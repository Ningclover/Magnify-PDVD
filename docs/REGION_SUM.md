# Region Sum Feature

Sums deconvolved waveforms over a trapezoidal region in (channel, tick) space
for each of the U, V, W planes independently, then overlays the three 1D
summed waveforms on a shared canvas for direct comparison.

---

## Motivation

A single charged particle track crosses all three wire planes at roughly the
same drift time.  To compare the collected charge per plane one needs to
integrate the signal along the track trajectory, which in the 2D
(channel × tick) view appears as a tilted strip — not a simple rectangle.
The region-sum tool lets you draw a trapezoid that follows the track and
accumulate the charge in each plane independently.

---

## Opening the tool

Click **Region Sum** in the General group of the main control panel.
A floating window appears with:

- A **Region Selection** panel (one row per plane)
- An embedded ROOT canvas showing the summed histograms after **Sum** is clicked

The window persists when closed (just hides); re-clicking "Region Sum"
brings it back with all previous values intact.

---

## Region definition

Each plane (U, V, W) has its own set of six parameters:

| Parameter | Widget label | Meaning |
|-----------|-------------|---------|
| `ch_start` | first number in the plane row | Global channel number at the start of the region |
| `t_low_s`  | "t start low"  | Lower tick bound at `ch_start` |
| `t_high_s` | "t start high" | Upper tick bound at `ch_start` |
| `ch_end`   | second channel number | Global channel number at the end of the region |
| `t_low_e`  | "t end low"  | Lower tick bound at `ch_end` |
| `t_high_e` | "t end high" | Upper tick bound at `ch_end` |

For channels between `ch_start` and `ch_end`, both the lower and upper tick
bounds are **linearly interpolated**:

```
t_low(ch)  = t_low_s  + frac × (t_low_e  − t_low_s)
t_high(ch) = t_high_s + frac × (t_high_e − t_high_s)
    where frac = (ch − ch_start) / (ch_end − ch_start)
```

This produces a trapezoidal selection that can follow an angled track.
Setting identical values at start and end degenerates to a rectangle.

**Note:** the time parameters are independent per plane.  Because different
wire planes see the same ionisation at slightly different nominal positions,
you can set a slightly different tick window per plane if needed.  The summed
1D histogram X-axis covers the union of all three planes' ranges so the curves
remain aligned for direct comparison.

---

## Filling the parameters

### Typing directly
Edit any `TGNumberEntry` field in the Region Selection panel by hand.

### Click-to-fill
1. Click **Set Start** — the button enters capture mode (stdout prints a
   confirmation message).
2. Click anywhere on one of the three **deconvolved** 2D pads (pads 4 / 5 / 6
   for U / V / W).  The clicked channel populates `ch_start` for that plane;
   the clicked tick seeds both `t_low_s` and `t_high_s` for that same plane.
3. Widen `t_high_s` by editing the widget.
4. Repeat step 2 for the other two planes while Set Start is still active.
5. Click **Set Start** again to exit capture mode.
6. Repeat steps 1–5 with **Set End** to fill the end-channel parameters.

Capture mode stays active until you click the mode button a second time, so
you can populate all three planes with one activation.

---

## Buttons

| Button | Action |
|--------|--------|
| **Set Start** | Toggle capture mode: next click on a decon pad fills `ch_start` + `t_low/high_s` for that plane |
| **Set End**   | Toggle capture mode: next click on a decon pad fills `ch_end` + `t_low/high_e` for that plane |
| **Sum**       | Compute and draw summed 1D histograms in the embedded canvas |
| **Draw**      | Overlay the trapezoidal boundary (4 dashed lines per plane) on the three 2D decon pads so you can visually verify the selection; re-clicking redraws with current values |
| **Erase**     | Remove the boundary lines from the 2D pads without clearing the widget values |
| **Clear**     | Erase boundary lines, zero all widgets, and clear the histogram canvas |

---

## Summed histogram display

After clicking **Sum** the embedded canvas shows three overlaid `TH1F`
curves on a common tick axis:

| Curve colour | Plane |
|-------------|-------|
| Red         | U     |
| Blue        | V     |
| Green       | W     |

A legend in the upper-right corner shows which channel range each curve
covers.  The Y-axis is auto-scaled to the global extremum across all three
planes.  The X-axis spans the union of all three planes' tick ranges.

The per-bin value added to the sum for channel `ch` and tick `j` is:

```
hOrig->GetBinContent(ch_bin, j) × fScale
```

where `fScale` is the deconvolved display scale factor (same as used for the
2D colour map, `1/(100 × rebin/4)`).  Ticks outside the interpolated
`[t_low(ch), t_high(ch)]` window for that channel are excluded.

---

## Visual verification with Draw / Erase

Click **Draw** after filling the parameters (before or after clicking Sum)
to overlay the selection boundary on each decon pad:

- **Left edge**: vertical dashed line at `ch_start` spanning `[t_low_s, t_high_s]`
- **Right edge**: vertical dashed line at `ch_end` spanning `[t_low_e, t_high_e]`
- **Top edge**: diagonal dashed line from `(ch_start, t_high_s)` to `(ch_end, t_high_e)`
- **Bottom edge**: diagonal dashed line from `(ch_start, t_low_s)` to `(ch_end, t_low_e)`

Lines are drawn in dashed orange.  Click **Draw** again after adjusting
values to redraw (automatically erases the previous set first).
Click **Erase** to remove the lines without touching the widget values or
the histogram canvas.

---

## Implementation notes

| Symbol | Location | Purpose |
|--------|----------|---------|
| `GuiController::ShowRegionWindow()` | `viewer/GuiController.cc` | Lazily constructs the `TGMainFrame` pop-up (once) and maps/raises it |
| `GuiController::SumRegion()` | `viewer/GuiController.cc` | Reads widgets, loops over channel/tick bins with trapezoid interpolation, draws into `regionCanvas` |
| `GuiController::DrawRegion()` | `viewer/GuiController.cc` | Creates 4 `TLine` objects per plane on `vw->can` pads 4–6 |
| `GuiController::EraseRegion()` | `viewer/GuiController.cc` | Removes stored `TLine` objects from pad primitive lists |
| `RegionCaptureMode` enum | `viewer/GuiController.h` | `CAPTURE_NONE / CAPTURE_START / CAPTURE_END` — governs click-to-fill behaviour |
| `regionBoundary[3][4]` | `viewer/GuiController.h` | Persistent `TLine*` pointers used by Erase to remove exact primitives |
| `regChStart/End[3]`, `regTLow/HighS/E[3]` | `viewer/GuiController.h` | Per-plane `TGNumberEntry*` pointers, valid after first `ShowRegionWindow()` call |
