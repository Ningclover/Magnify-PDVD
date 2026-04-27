"""Convert WireCell FrameFileSink tar.bz2 archives to a Magnify-ready ROOT file.

Two modes
---------
manual mode (default)
    Supply individual archive files explicitly.  Each tag is written as
    hu_<tag><N>, hv_<tag><N>, hw_<tag><N> — the per-APA convention that
    preprocess.C expects.

    python frames_to_root.py --detector hd --tag gauss --tag wiener \\
        evt_1/protodunehd-sp-frames-anode{0,1,2,3}.tar.bz2 \\
        --out magnify.root

auto mode  (--auto)
    Point at an event folder.  The script discovers all archives, converts
    every anode, and stitches the per-APA histograms into whole-detector
    histograms (hu_orig, hv_raw, hw_decon, …) ready to open directly in
    magnify.sh — no preprocess.sh step needed.

    python frames_to_root.py --auto --detector hd \\
        /path/to/hd/run027409/evt_1/

    python frames_to_root.py --auto --detector vd \\
        /path/to/vd/run039324/evt_1/

    Output is written to the same folder as the input:
        <folder>/magnify-hd-<run>-<evt>.root   (HD)
        <folder>/magnify-vd-<run>-<evt>.root   (VD)
"""

from __future__ import annotations

import argparse
import glob
import io
import os
import re
import sys
import tarfile

import numpy as np


# ── detector geometry ─────────────────────────────────────────────────────────

# Channels per plane (U, V, W) within one APA.
# HD: fixed split (channels are contiguous across planes within an APA).
# VD: None → auto-detect split points from gaps in channel numbers.
DETECTOR_PLANES = {
    "hd": [800, 800, 960],  # 800 U + 800 V + 960 W = 2560 ch/APA
    "vd": None,             # planes separated by channel-number gaps (~476/476/584)
}

DETECTOR_N_ANODES = {"hd": 4, "vd": 8}

PLANE_LABELS = ["u", "v", "w"]

# How each logical data type maps to archive filename patterns and internal tag.
# Each entry: (filename_glob_fragment, internal_base_tag, output_tag)
#   filename_glob_fragment: substring to match in archive filename
#   internal_base_tag:      tag name embedded in the npy keys (before anode digit)
#                           '*' means the archive uses a literal '*' as tag (HD orig)
#   output_tag:             histogram name tag written to ROOT (e.g. 'gauss' → 'decon')
HD_ARCHIVE_TYPES = [
    # (filename fragment,          internal tag,  output tag)
    ("sp-frames-anode",            "gauss",       "gauss"),
    ("sp-frames-anode",            "wiener",      "wiener"),
    ("sp-frames-raw-anode",        "raw",         "raw"),
    ("orig-frames-anode",          "*",           "orig"),
]

VD_ARCHIVE_TYPES = [
    ("sp-frames-anode",            "gauss",       "gauss"),
    ("sp-frames-anode",            "wiener",      "wiener"),
    # VD has both protodune-raw-frames-* and protodune-sp-frames-raw-*
    # prefer sp-frames-raw (signal-processing raw) when both exist
    ("sp-frames-raw-anode",        "raw",         "raw"),
    ("orig-frames-anode",          "orig",        "orig"),
]

# Whole-detector output dimensions per detector (xmin, xmax, nticks)
# These are the axes of the final stitched histograms.
DETECTOR_DIMS = {
    #         xmin   xmax      nticks
    "hd": (  -0.5, 10239.5,   6000),   # 4 APAs × 2560 = 10240 channels
    "vd": (  -0.5, 12287.5,   6400),   # 8 APAs × 1536 = 12288 channels (gaps excluded)
}


# ── archive helpers ────────────────────────────────────────────────────────────

def _load_archive(path: str) -> dict:
    data = {}
    with tarfile.open(path, "r:bz2") as tf:
        for member in tf.getmembers():
            if member.name.endswith(".npy"):
                raw = tf.extractfile(member).read()
                data[member.name[:-4]] = np.load(io.BytesIO(raw))
    return data


def _find_tags(raw_data: dict, anode_no: int) -> list[str]:
    """Return base tag names (anode digit stripped) from frame_<tag><N>_<ident> keys."""
    tag_re = re.compile(r"^frame_(.+)_\d+$")
    seen: list[str] = []
    for k in raw_data:
        m = tag_re.match(k)
        if m:
            raw_tag = m.group(1)  # e.g. "gauss0", "raw1", "*"
            base = re.sub(rf"{anode_no}$", "", raw_tag)
            if base not in seen:
                seen.append(base)
    return seen


def _infer_anode(path: str) -> int:
    """Parse anode index from filename, e.g. 'protodunehd-sp-frames-anode3.tar.bz2' → 3."""
    m = re.search(r"anode(\d+)", os.path.basename(path))
    if m:
        return int(m.group(1))
    raise ValueError(f"Cannot infer anode number from filename: {path}")


# ── plane splitting ────────────────────────────────────────────────────────────

def _split_planes(frame: np.ndarray, channels: np.ndarray,
                  plane_sizes: list[int] | None):
    """Split (nch, ntick) frame into three plane slices.

    plane_sizes: fixed per-plane channel counts (HD).
                 None → auto-detect from channel-number gaps (VD).
    """
    if plane_sizes is not None:
        starts = [0]
        for size in plane_sizes[:-1]:
            starts.append(starts[-1] + size)
        ends = starts[1:] + [len(channels)]
    else:
        gap_idx = list(np.where(np.diff(channels) > 1)[0])
        starts = [0] + [i + 1 for i in gap_idx]
        ends   = [i + 1 for i in gap_idx] + [len(channels)]

    slices = [(frame[s:e], channels[s:e]) for s, e in zip(starts, ends)]

    nticks = frame.shape[1] if frame.ndim == 2 else 1
    while len(slices) < 3:
        slices.append((np.zeros((1, nticks), dtype=frame.dtype), np.array([0])))
    return slices[:3]


# ── single-archive conversion (manual mode) ───────────────────────────────────

def _write_tag_per_apa(tfile, base_tag: str, anode_no: int, raw_data: dict,
                       plane_sizes: list[int] | None) -> bool:
    """Write hu_<tag><N>, hv_<tag><N>, hw_<tag><N> for one APA into tfile."""
    import ROOT

    candidates = [f"{base_tag}{anode_no}", base_tag, "*"]
    frame_key = ch_key = ti_key = None
    for candidate in candidates:
        pat = rf"^frame_{re.escape(candidate)}_\d+$"
        fk = next((k for k in raw_data if re.match(pat, k)), None)
        if fk:
            frame_key = fk
            ch_key = next(
                (k for k in raw_data
                 if re.match(rf"^channels_{re.escape(candidate)}_\d+$", k)), None)
            ti_key = next(
                (k for k in raw_data
                 if re.match(rf"^tickinfo_{re.escape(candidate)}_\d+$", k)), None)
            break

    if frame_key is None:
        print(f"  WARNING: frame_{base_tag}{anode_no}_* not found — skipping")
        return False

    frame    = raw_data[frame_key]
    channels = raw_data[ch_key] if ch_key else np.arange(frame.shape[0])
    tickinfo = raw_data[ti_key] if ti_key else np.array([0.0, frame.shape[1], 0.5])

    start_tick = int(tickinfo[0])
    nticks     = frame.shape[1]
    end_tick   = start_tick + nticks

    print(f"  Tag '{base_tag}' (key={frame_key}) → '{base_tag}{anode_no}': "
          f"{len(channels)} ch, ticks {start_tick}-{end_tick}")

    planes = _split_planes(frame.astype(np.float64), channels, plane_sizes)

    for (plane_frame, plane_ch), plane_lbl in zip(planes, PLANE_LABELS):
        nch    = len(plane_ch)
        ch_min = int(plane_ch[0])
        ch_max = int(plane_ch[-1])
        hist_name = f"h{plane_lbl}_{base_tag}{anode_no}"

        print(f"    Plane {plane_lbl.upper()}: ch {ch_min}-{ch_max} ({nch} ch) "
              f"→ '{hist_name}'")

        if base_tag == "orig":
            h = ROOT.TH2I(hist_name, hist_name,
                          nch, ch_min - 0.5, ch_max + 0.5,
                          nticks, start_tick, end_tick)
        else:
            h = ROOT.TH2F(hist_name, hist_name,
                          nch, ch_min - 0.5, ch_max + 0.5,
                          nticks, start_tick, end_tick)
        h.SetDirectory(tfile)

        for col, ch_val in enumerate(plane_ch):
            xbin = h.GetXaxis().FindBin(int(ch_val))
            for tick_i, val in enumerate(plane_frame[col]):
                h.SetBinContent(xbin, tick_i + 1, float(val))

        h.Write()

    return True


# ── whole-detector stitching (auto mode) ──────────────────────────────────────

def _stitch_tag(archives_by_anode: dict[int, str], internal_tag: str,
                output_tag: str, plane_sizes: list[int] | None,
                det_dims: tuple, tfile, file_mode_first: list[bool],
                baseline_subtract: bool = False) -> bool:
    """
    Load all per-anode archives for one data type and stitch them into three
    whole-detector TH2 histograms (hu_<output_tag>, hv_<output_tag>, hw_<output_tag>).

    Returns True if at least one anode contributed data.
    """
    import ROOT

    xmin, xmax, nticks = det_dims

    # Create the three whole-detector histograms
    nbinsx = round(xmax - xmin)
    if output_tag == "orig":
        hu = ROOT.TH2I(f"hu_{output_tag}", f"hu_{output_tag}",
                       nbinsx, xmin, xmax, nticks, 0, nticks)
        hv = ROOT.TH2I(f"hv_{output_tag}", f"hv_{output_tag}",
                       nbinsx, xmin, xmax, nticks, 0, nticks)
        hw = ROOT.TH2I(f"hw_{output_tag}", f"hw_{output_tag}",
                       nbinsx, xmin, xmax, nticks, 0, nticks)
    else:
        hu = ROOT.TH2F(f"hu_{output_tag}", f"hu_{output_tag}",
                       nbinsx, xmin, xmax, nticks, 0, nticks)
        hv = ROOT.TH2F(f"hv_{output_tag}", f"hv_{output_tag}",
                       nbinsx, xmin, xmax, nticks, 0, nticks)
        hw = ROOT.TH2F(f"hw_{output_tag}", f"hw_{output_tag}",
                       nbinsx, xmin, xmax, nticks, 0, nticks)

    hall = [hu, hv, hw]
    any_written = False

    for anode_no in sorted(archives_by_anode):
        path = archives_by_anode[anode_no]
        print(f"  [anode {anode_no}] {os.path.basename(path)}")
        raw_data = _load_archive(path)

        # Find the frame key: try <internal_tag><anode_no>, <internal_tag>, '*'
        candidates = [f"{internal_tag}{anode_no}", internal_tag, "*"]
        frame_key = ch_key = ti_key = None
        for candidate in candidates:
            pat = rf"^frame_{re.escape(candidate)}_\d+$"
            fk = next((k for k in raw_data if re.match(pat, k)), None)
            if fk:
                frame_key = fk
                ch_key = next(
                    (k for k in raw_data
                     if re.match(rf"^channels_{re.escape(candidate)}_\d+$", k)), None)
                ti_key = next(
                    (k for k in raw_data
                     if re.match(rf"^tickinfo_{re.escape(candidate)}_\d+$", k)), None)
                break

        if frame_key is None:
            print(f"    WARNING: tag '{internal_tag}' not found in archive — skipping anode {anode_no}")
            continue

        frame    = raw_data[frame_key]
        channels = raw_data[ch_key] if ch_key else np.arange(frame.shape[0])
        tickinfo = raw_data[ti_key] if ti_key else np.array([0.0, frame.shape[1], 0.5])

        start_tick = int(tickinfo[0])
        n_ticks    = frame.shape[1]
        print(f"    {len(channels)} ch, ticks {start_tick}-{start_tick + n_ticks}")

        planes = _split_planes(frame.astype(np.float64), channels, plane_sizes)

        for (plane_frame, plane_ch), h in zip(planes, hall):
            # baseline subtraction for raw/denoised: subtract per-channel mode
            if baseline_subtract:
                baseline_hist = np.zeros(4096)
                for col in range(len(plane_ch)):
                    for val in plane_frame[col]:
                        idx = int(val)
                        if 0 <= idx < 4096:
                            baseline_hist[idx] += 1

            for col, ch_val in enumerate(plane_ch):
                xbin = h.GetXaxis().FindBin(int(ch_val))
                col_data = plane_frame[col].copy()

                if baseline_subtract:
                    # per-channel mode baseline
                    ch_hist = np.zeros(4096)
                    for val in col_data:
                        idx = int(val)
                        if 0 <= idx < 4096:
                            ch_hist[idx] += 1
                    baseline = float(np.argmax(ch_hist))
                    col_data = col_data - baseline

                for tick_i, val in enumerate(col_data):
                    ybin = tick_i + 1  # ticks always start at bin 1 in whole-det histo
                    h.SetBinContent(xbin, ybin, float(val))

        any_written = True

    if not any_written:
        print(f"  WARNING: no data written for tag '{output_tag}'")
        return False

    tfile.cd()
    hu.Write()
    hv.Write()
    hw.Write()
    print(f"  → wrote hu_{output_tag}, hv_{output_tag}, hw_{output_tag}")
    return True


# ── auto mode: discover archives in a folder ──────────────────────────────────

def _discover_archives(folder: str, detector: str
                       ) -> dict[str, dict[int, str]]:
    """
    Scan folder for per-anode archives and group them by data type.

    Returns dict keyed by output_tag → {anode_no: path}.
    Missing anodes are simply absent from the inner dict.
    """
    archive_types = HD_ARCHIVE_TYPES if detector == "hd" else VD_ARCHIVE_TYPES
    n_anodes = DETECTOR_N_ANODES[detector]

    # Build a lookup: output_tag → {anode: path}
    # Multiple archive_types entries can share the same output_tag (e.g. gauss+wiener
    # both come from sp-frames); we keep the first match per (output_tag, anode).
    result: dict[str, dict[int, str]] = {}
    all_archives = sorted(glob.glob(os.path.join(folder, "*.tar.bz2")))

    for fname_fragment, internal_tag, output_tag in archive_types:
        if output_tag not in result:
            result[output_tag] = {}
        for anode_no in range(n_anodes):
            if anode_no in result[output_tag]:
                continue  # already found from a previous archive_types entry
            pattern = f"*{fname_fragment}{anode_no}.tar.bz2"
            matches = [p for p in all_archives
                       if re.search(
                           rf"{re.escape(fname_fragment)}{anode_no}\.tar\.bz2$",
                           os.path.basename(p)
                       )]
            # Exclude sparseon variants
            matches = [p for p in matches if "sparseon" not in os.path.basename(p)]
            if matches:
                result[output_tag][anode_no] = matches[0]

    return result


def _infer_run_evt(folder: str) -> tuple[str, str]:
    """Try to infer run and event identifiers from the folder path."""
    parts = os.path.normpath(folder).split(os.sep)
    run = evt = ""
    for part in parts:
        if re.match(r"run\d+", part):
            run = part
        elif re.match(r"evt_?\d+", part):
            evt = part
    return run, evt


OUTPUT_TAGS = {
    # key: (internal_tag, output_tag, baseline_subtract)
    "gauss":  ("gauss",  "decon",  False),   # gauss → renamed decon
    "wiener": ("wiener", "wiener", False),
    "raw":    ("raw",    "raw",    True),     # raw   → baseline-subtracted
    "orig":   ("orig",   "orig",   False),
}


def _write_single_anode(anode_no: int, by_tag: dict, plane_sizes: list | None,
                        out_path: str) -> None:
    """Write one ROOT file for a single anode with hu/hv/hw_<tag> histograms."""
    import ROOT

    tfile = ROOT.TFile(out_path, "RECREATE")
    if tfile.IsZombie():
        print(f"  ERROR: could not open {out_path}", file=sys.stderr)
        return

    any_tag_written = False
    for key, (internal_tag, output_tag, baseline_sub) in OUTPUT_TAGS.items():
        anode_map = by_tag.get(key, {})
        if anode_no not in anode_map:
            print(f"  WARNING: '{key}' not found for anode {anode_no} — skipping")
            continue

        path = anode_map[anode_no]
        print(f"  [{output_tag}] {os.path.basename(path)}")
        raw_data = _load_archive(path)

        candidates = [f"{internal_tag}{anode_no}", internal_tag, "*"]
        frame_key = ch_key = ti_key = None
        for candidate in candidates:
            pat = rf"^frame_{re.escape(candidate)}_\d+$"
            fk = next((k for k in raw_data if re.match(pat, k)), None)
            if fk:
                frame_key = fk
                ch_key = next(
                    (k for k in raw_data
                     if re.match(rf"^channels_{re.escape(candidate)}_\d+$", k)), None)
                ti_key = next(
                    (k for k in raw_data
                     if re.match(rf"^tickinfo_{re.escape(candidate)}_\d+$", k)), None)
                break

        if frame_key is None:
            print(f"    WARNING: tag '{internal_tag}' not found in archive — skipping")
            continue

        frame    = raw_data[frame_key]
        channels = raw_data[ch_key] if ch_key else np.arange(frame.shape[0])
        tickinfo = raw_data[ti_key] if ti_key else np.array([0.0, frame.shape[1], 0.5])
        start_tick = int(tickinfo[0])
        nticks     = frame.shape[1]
        end_tick   = start_tick + nticks
        print(f"    {len(channels)} ch, ticks {start_tick}-{end_tick}")

        planes = _split_planes(frame.astype(np.float64), channels, plane_sizes)

        for (plane_frame, plane_ch), plane_lbl in zip(planes, PLANE_LABELS):
            nch    = len(plane_ch)
            ch_min = int(plane_ch[0])
            ch_max = int(plane_ch[-1])
            hist_name = f"h{plane_lbl}_{output_tag}"

            if output_tag == "orig":
                h = ROOT.TH2I(hist_name, hist_name,
                              nch, ch_min - 0.5, ch_max + 0.5,
                              nticks, start_tick, end_tick)
            else:
                h = ROOT.TH2F(hist_name, hist_name,
                              nch, ch_min - 0.5, ch_max + 0.5,
                              nticks, start_tick, end_tick)
            h.SetDirectory(tfile)

            for col, ch_val in enumerate(plane_ch):
                xbin = h.GetXaxis().FindBin(int(ch_val))
                col_data = plane_frame[col].copy()
                if baseline_sub:
                    ch_hist = np.zeros(4096)
                    for val in col_data:
                        idx = int(val)
                        if 0 <= idx < 4096:
                            ch_hist[idx] += 1
                    col_data = col_data - float(np.argmax(ch_hist))
                for tick_i, val in enumerate(col_data):
                    h.SetBinContent(xbin, tick_i + 1, float(val))

            h.Write()
            print(f"    → h{plane_lbl}_{output_tag} ({nch} ch × {nticks} ticks)")

        any_tag_written = True

    tfile.Close()
    if any_tag_written:
        print(f"  Saved → {out_path}")
    else:
        os.remove(out_path)
        print(f"  (no data — removed {out_path})")


def run_auto(args: argparse.Namespace) -> None:
    """Auto mode: process an entire event folder."""
    import ROOT
    ROOT.gROOT.SetBatch(True)

    folder      = os.path.realpath(args.folder)
    detector    = args.detector
    n_anodes    = DETECTOR_N_ANODES[detector]
    plane_sizes = DETECTOR_PLANES[detector]
    det_dims    = DETECTOR_DIMS[detector]
    split       = args.split_anode

    print(f"Auto mode — detector={detector}, folder={folder}")
    print(f"Expected anodes: 0-{n_anodes - 1}")
    print(f"Output mode: {'one file per anode' if split else 'single stitched whole-detector file'}")

    by_tag = _discover_archives(folder, detector)

    print("\nArchive discovery:")
    for output_tag, anode_map in sorted(by_tag.items()):
        found   = sorted(anode_map)
        missing = [i for i in range(n_anodes) if i not in anode_map]
        status  = f"anodes found: {found}"
        if missing:
            status += f"  WARNING missing: {missing}"
        print(f"  {output_tag:12s}  {status}")

    run, evt = _infer_run_evt(folder)
    label = "-".join(filter(None, [detector, run, evt])) or "data"

    if split:
        # ── per-anode mode ────────────────────────────────────────────────────
        all_anodes = sorted({a for m in by_tag.values() for a in m})
        print(f"\nWriting {len(all_anodes)} per-anode files...")
        for anode_no in all_anodes:
            out_path = os.path.join(folder, f"magnify-{label}-anode{anode_no}.root")
            print(f"\n[anode {anode_no}] → {os.path.basename(out_path)}")
            _write_single_anode(anode_no, by_tag, plane_sizes, out_path)
        print("\nDone.")
        print("To view a single anode:  "
              f"./magnify.sh {os.path.join(folder, f'magnify-{label}-anode0.root')} 30 decon 1")

    else:
        # ── whole-detector stitched mode ──────────────────────────────────────
        out_path = os.path.join(folder, f"magnify-{label}.root")
        tfile = ROOT.TFile(out_path, "RECREATE")
        if tfile.IsZombie():
            print(f"ERROR: could not open {out_path}", file=sys.stderr)
            sys.exit(1)

        print(f"\nOutput: {out_path}\n")
        file_mode_first = [True]

        for key, (internal_tag, output_tag, baseline_sub) in OUTPUT_TAGS.items():
            anode_map = by_tag.get(key, {})
            if not anode_map:
                print(f"\nWARNING: no archives found for '{key}' — skipping")
                continue
            print(f"\nStitching '{key}' → hu/hv/hw_{output_tag} ...")
            _stitch_tag(anode_map, internal_tag, output_tag, plane_sizes,
                        det_dims, tfile, file_mode_first,
                        baseline_subtract=baseline_sub)

        tfile.Close()
        print(f"\nDone → {out_path}")
        print(f"To view:  ./magnify.sh {out_path} 30 decon 1")


# ── manual mode ───────────────────────────────────────────────────────────────

def run_manual(args: argparse.Namespace) -> None:
    """Manual mode: convert explicitly listed archives."""
    import ROOT
    ROOT.gROOT.SetBatch(True)

    paths: list[str] = []
    for pattern in args.frame_files:
        expanded = sorted(glob.glob(pattern))
        if expanded:
            paths.extend(expanded)
        elif os.path.exists(pattern):
            paths.append(pattern)
        else:
            print(f"WARNING: no files matched '{pattern}'", file=sys.stderr)
    if not paths:
        print("ERROR: no input files found.", file=sys.stderr)
        sys.exit(1)

    plane_sizes = DETECTOR_PLANES[args.detector]
    file_mode   = "UPDATE" if args.append else "RECREATE"
    tfile = ROOT.TFile(args.out, file_mode)
    if tfile.IsZombie():
        print(f"ERROR: could not open {args.out}", file=sys.stderr)
        sys.exit(1)

    print(f"Output: {args.out}  (mode={file_mode}, detector={args.detector})")
    print(f"Plane sizes (U/V/W): {plane_sizes}")

    for path in paths:
        anode_no = _infer_anode(path)
        print(f"\n[anode {anode_no}] {os.path.basename(path)}")

        raw_data  = _load_archive(path)
        found     = _find_tags(raw_data, anode_no)
        print(f"  Archive base tags: {found}")

        requested = args.tags if args.tags else found
        for tag in requested:
            _write_tag_per_apa(tfile, tag, anode_no, raw_data, plane_sizes)

    tfile.Close()
    print(f"\nDone → {args.out}")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--detector", default="hd", choices=["hd", "vd"],
        help="Detector geometry: 'hd' (ProtoDUNE-HD, 4 anodes) or "
             "'vd' (ProtoDUNE-VD, 8 anodes). Default: hd",
    )

    sub = p.add_subparsers(dest="mode")

    # ── auto sub-command ──
    pa = sub.add_parser(
        "auto",
        help="Discover all archives in a folder and produce ready-to-view ROOT file(s)",
    )
    pa.add_argument("folder", help="Event folder containing *.tar.bz2 archives")
    pa.add_argument(
        "--split-anode", action="store_true",
        help="Write one ROOT file per anode (magnify-*-anode<N>.root) instead of "
             "a single stitched whole-detector file",
    )
    pa.set_defaults(func=run_auto)

    # ── manual sub-command (default) ──
    pm = sub.add_parser(
        "manual",
        help="Convert explicitly listed archive files (per-APA histogram naming)",
    )
    pm.add_argument("frame_files", nargs="+",
                    help="One or more per-anode *.tar.bz2 archives")
    pm.add_argument("--tag", action="append", dest="tags", default=None,
                    metavar="TAG",
                    help="Tag(s) to extract (default: all found in archive)")
    pm.add_argument("--out", required=True, help="Output ROOT file path")
    pm.add_argument("--append", action="store_true",
                    help="Append to existing ROOT file instead of recreating")
    pm.set_defaults(func=run_manual)

    args = p.parse_args(argv)

    # Allow bare invocation without subcommand for backward compatibility:
    # if first positional looks like a file/glob, treat as manual mode.
    if args.mode is None:
        # Re-parse forcing manual sub-command
        argv2 = list(argv or sys.argv[1:])
        if argv2 and not argv2[0].startswith("-") and argv2[0] not in ("auto", "manual"):
            argv2 = ["manual"] + argv2
        else:
            p.print_help()
            sys.exit(0)
        args = p.parse_args(argv2)

    try:
        import ROOT  # noqa: F401 — early check before any work
    except ImportError:
        print("ERROR: PyROOT is required. Activate a ROOT-enabled environment.",
              file=sys.stderr)
        sys.exit(1)

    args.func(args)


if __name__ == "__main__":
    main()
