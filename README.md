# Magnify-PDVD

A waveform viewer for ProtoDUNE detector data (HD and VD), built on ROOT.

---

## 1. Convert Data to ROOT

Input data arrives as per-anode `.tar.bz2` archives containing NumPy frames
produced by Wire-Cell Toolkit.  The conversion script
`scripts/frames_to_root.py` turns these into ROOT files that the viewer can
open directly.  It requires Python with `numpy` and `ROOT` (PyROOT) available.

### Auto mode — whole folder at once (recommended)

Point the script at an event folder.  It discovers all archives automatically,
converts every anode, and writes one ready-to-view ROOT file.

**ProtoDUNE-HD** (4 anodes, 0–3):
```bash
python scripts/frames_to_root.py --detector hd auto \
    /path/to/hd/run027409/evt_1/
# output: /path/to/hd/run027409/evt_1/magnify-hd-run027409-evt_1.root
```

**ProtoDUNE-VD** (8 anodes, 0–7):
```bash
python scripts/frames_to_root.py --detector vd auto \
    /path/to/vd/run039324/evt_1/
# output: /path/to/vd/run039324/evt_1/magnify-vd-run039324-evt_1.root
```

The output file is saved in the same folder as the input archives.

### Split mode — one file per anode

Add `--split-anode` to produce a separate ROOT file for each anode instead of
one stitched whole-detector file.  Each per-anode file can be opened directly
in the viewer.

```bash
python scripts/frames_to_root.py --detector hd auto --split-anode \
    /path/to/hd/run027409/evt_1/
# output: magnify-hd-run027409-evt_1-anode0.root
#         magnify-hd-run027409-evt_1-anode1.root
#         magnify-hd-run027409-evt_1-anode2.root
#         magnify-hd-run027409-evt_1-anode3.root
```

### What gets converted

| Data type | Source archive pattern | Output histogram tag |
|-----------|------------------------|----------------------|
| Raw ADC (orig) | `*-orig-frames-anode<N>.tar.bz2` | `orig` |
| Denoised (raw) | `*-sp-frames-raw-anode<N>.tar.bz2` | `raw` (baseline-subtracted) |
| Deconvoluted (gauss) | `*-sp-frames-anode<N>.tar.bz2` | `decon` |
| Wiener-filtered | `*-sp-frames-anode<N>.tar.bz2` | `wiener` |

If any archive is missing, a warning is printed and the remaining types are
still converted.

---

## 2. Build Magnify (first time, or after source changes)

Magnify compiles its C++ source files at runtime via ROOT's ACLiC system.
Run `build.sh` once to compile and prepare all shared libraries before
launching the viewer for the first time.

```bash
cd /path/to/Magnify-PDVD
./build.sh
```

**When to rebuild:**

| Situation | Action |
|-----------|--------|
| First time running Magnify on this machine | `./build.sh` |
| After `git pull` that changed any `.cc` or `.h` file | `./build.sh` |
| After manually editing any source file in `event/` or `viewer/` | `./build.sh` |
| Build fails or viewer crashes on startup with symbol errors | `./build.sh --force` |

`build.sh` is incremental — it only recompiles files whose source is newer
than their compiled `.so`.  Use `--force` to recompile everything from scratch.

> **Note (macOS 15+ with conda ROOT):** the build produces harmless linker
> warnings (`duplicate LC_RPATH`, `_main` undefined).  These are suppressed by
> `build.sh` and do not affect the viewer.  The script fixes the underlying
> issue automatically.

---

## 3. Run the Viewer

```bash
./magnify.sh /path/to/file.root
# or with explicit options:
./magnify.sh /path/to/file.root <threshold> <frame> <rebin>
```

**Arguments:**

| Argument | Default | Meaning |
|----------|---------|---------|
| `threshold` | `30` | ADC cut for drawing a signal box in the 2D view |
| `frame` | `decon` | Which signal processing output to display: `decon`, `wiener`, `raw`, `orig` |
| `rebin` | `1` | Tick rebin factor (1 = full resolution, 4 = 4× compressed) |

### Typical launch commands

```bash
# Whole-detector stitched file (all anodes merged)
./magnify.sh ../../data_pd/hd/run027409/evt_1/magnify-hd-run027409-evt_1.root

# Single anode file
./magnify.sh ../../data_pd/hd/run027409/evt_1/magnify-hd-run027409-evt_1-anode0.root

# View raw ADC instead of deconvoluted
./magnify.sh /path/to/file.root 30 orig 1

# View Wiener output, compressed ticks
./magnify.sh /path/to/file.root 30 wiener 4
```

If no file is given, a file-picker dialog opens:
```bash
cd scripts/
root -l loadClasses.C Magnify.C
```

### Event / Anode Navigation

The control window has a **Navigation** group for switching between events and
anodes without restarting ROOT:

| Widget | Action |
|--------|--------|
| `anode` combo | Switch to a different anode of the current event |
| `event` combo | Jump to any discovered event |
| `<` / `>` buttons | Previous / next event |

Events are auto-discovered by scanning the parent directory for folders
matching the `<run>_<event>` naming pattern that contain `magnify-*.root` files.

---

## 4. Other Tools

### Per-Channel RMS Noise Analysis

Computes per-channel noise RMS in batch (no display).  Output is written
alongside each input as `<file>.rms.root` and loaded automatically by the
viewer on next startup.

```bash
./scripts/run_rms_analysis.sh /path/to/magnify-*.root
```

### Channel Scan (experimental)

```bash
./channelscan.sh /path/to/file.root
```

Loops over channels defined in a bad-channel tree or text file.  See
`./channelscan.sh -h` for options.
