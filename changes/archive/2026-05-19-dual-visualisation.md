# Dual visualisation overlay

**Mode:** Explore

## Intent

Show the waveform and heatmap simultaneously in the browser slot, ~2:3 height ratio, both reading the same audible-now reference frame at the right edge. `Ctrl+W` and `Ctrl+H` become independent toggles — both-on gives the dual layout. The waveform's existing audio look-ahead (playhead-at-20 %) is dropped so the two views align in time.

Open design: which view on top, exact proportions, what to do with the now-unused waveform playhead, whether the leftover wider-ring / virt-abs scaffolding from the shelved waveform-widening attempt gets reused or removed.

## Approach

### Layout: waveform 2/5 top, heatmap 3/5 bottom

Waveform's min/max envelope reads as a top-strip context, heatmap is the larger main content. The 2/5 vs 3/5 split matches the user's stated ratio; heatmap getting the larger share also gives its 28 bins a chance at legible bin heights.

### Shared time base via `disp_abs`

Both views are driven by the same wall-clock-paced cursor. The heatmap's existing mechanism becomes generic (renamed). The waveform's column rate is realigned to match the heatmap's (commit every `SPEC_COL_SAMPLES` samples) so both rings scroll in lockstep at exactly the same time-per-column.

### Waveform: drop look-ahead, drop playhead

Look-ahead-driven samples-per-col calibration goes. The right edge is "now". The 20 %-from-left playhead column is removed — at the right edge, "now" is implicit and a dedicated line is just noise.

### Wider waveform ring stays

The 0.19.9 prototype's 480-col waveform ring (`WV_COLS`) was correct in shape — it just lacked wall-clock pacing. Combined with the shared `disp_abs`, it provides the writer/reader headroom the heatmap proved necessary. Keep it; reuse the `abs_head` counter introduced then.

### Independent toggles, single overlay render path

`toggleWaveform` and `toggleHeatmap` stop turning each other off. `composeBrowser` dispatches to a single `composeVisualisationOverlay` when either is active; that compose draws whichever subset is enabled in its Y region. One pollVisualisation drives both via the shared `disp_abs`.

### Divider, single-view sizing, diagnostics-on layout

1-px hairline (browser frame colour) between the two views when both are active. Single-view modes (either alone) stretch to the full browser height — unchanged from current single-overlay behaviour. The 2/5 vs 3/5 dual layout applies even when diagnostics is open and the browser is short; the heatmap may be visibly squished (~36 px / 28 bins), and we'll judge on-device whether that's acceptable.

## Plan

Explore mode — topics rather than tasks.

**Topics**

- **Unify the pacing / render path.** Heatmap's `disp_abs` cursor, `pollHeatmap`, lookahead, and suppress-window mechanisms become generic — one `pollVisualisation` drives both rings via a shared cursor. `composeBrowser` dispatches to `composeVisualisationOverlay`, which draws the active subset(s) in their Y regions with a 1-px hairline divider when both are visible.

- **Waveform rewire.** Drop the look-ahead-driven `waveformSamplesPerCol` calibration; commit every `SPEC_COL_SAMPLES`. Drop the 20 %-from-left playhead. Keep the 480-col ring and `abs_head` from the 0.19.9 scaffolding, now driven by the shared cursor.

- **Independent toggles.** Remove the mutual exclusion in `toggleWaveform` / `toggleHeatmap`. Dual mode emerges from both-on; single-on stretches to full browser height.

- **On-device verdict.** Smoothness in dual mode (does both-view compose fit the column-rate budget?), legibility when diagnostics is open (heatmap squished to ~36 px), whether the views feel time-aligned.

**Done when**

Both visualisations render simultaneously, time-aligned, with no regressions to single-view smoothness. The diagnostics-on layout is assessed (keep, swap proportions, or hide-one if unusable). Written recommendation.

## Log

- Version bumped 0.20.27 → 0.21.0 (minor — substantive feature + behavioural change to existing waveform).
- audio_output_m5.h: `waveformSamplesPerCol()` now returns `SPEC_COL_SAMPLES`; the look-ahead-driven calibration that backed the playhead-at-20 % design is removed. The visualisation taps (waveform + spectrum) are gated by a single shared `_viz_suppress_samples` window so seeks blank both. `resetVisualisation()` (renamed from `resetSpectrum`) clears both rings.
- main.cpp: heatmap-specific state renamed to `g_viz_*`; old waveform-virt state and `pollWaveform` deleted. New `pollVisualisation` drives both views via the shared cursor. `composeVisualisationOverlay` lays out single-view (full inner) or dual-view (waveform 2/5 top, hairline divider, heatmap 3/5 below). Waveform playhead line removed. Toggles no longer mutually exclude. Ctrl+W / Ctrl+H in the overlay-active branch call their toggle functions instead of dismissing.
- Ready for first flash. Watch for: dual rendering working at all, time alignment between the two views, smoothness vs single-overlay baseline, legibility of the squished heatmap when diagnostics is open.
- 0.21.0 flashed and worked. User asked to zoom in. 0.21.1: `SPEC_COL_SAMPLES` 552 → 368 (3 s → 2 s on screen).
- 0.21.2: added "Auto heatmap" Settings toggle mirroring "Auto waveform" — independent persisted (`autohm`) flag, opens the heatmap overlay at non-paused track start. Track-start hook generalised to apply either or both `g_auto_*` flags.
- 0.21.3: removed hairline divider between waveform and heatmap in dual mode; heatmap gets the row back.

## Conclusion

Both views render simultaneously, time-aligned via a shared wall-clock-paced cursor, at 2 s on screen. Waveform's look-ahead and playhead are gone; both rings commit at the spectrum's column rate. Auto-show is per-view (Auto waveform / Auto heatmap), and `Ctrl+W` / `Ctrl+H` are independent. Diagnostics-on layout left as-is — squished but usable. No divider between views.

**Documentation impact:** new Heatmap Visualisation node under Browser; Waveform Visualisation needs updates (no playhead, no look-ahead, no mutual exclusion). Negotiate post-archive.
