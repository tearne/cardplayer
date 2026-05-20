# Panel-locked smooth scroll

**Mode:** Explore

## Intent

With the scroll-jitter rework in place, the visualisation render task fires at a precise nominal 60 Hz but the panel scan rate is somewhere between 60 and 61 Hz. The residual is a slow left-to-right tear sweep at the beat frequency.

Lock in the fine tuning, methodically:

- Add a `Ctrl+T` test pattern (evenly-spaced vertical bars) replacing the visualisation, so beat period and direction become directly measurable instead of inferred from the heatmap.
- Pin the panel's actual scan rate.
- Decide the optimal render rate. 60 fps may not be the right target — 30 fps with bigger scroll steps could be smoother, or worse.
- Consider whether tearing is best mitigated by exact rate-lock, by deliberate under-rendering, by changing how data updates feed into the sprite, or some combination.

## Approach

### Scrolling vertical-bar test pattern

`drawVizColIntoSprite` replaces audio-derived colour with a deterministic pattern when test mode is on: every Nth `col_abs` becomes a bright white column against black (N = 30 → 8 bars on a 240-wide screen). The bars scroll left with `disp_abs` just like real data. Tear becomes a visible break in the bar grid; direction and beat period are stopwatch-measurable.

### `Ctrl+T` toggles test mode

Pure diagnostic. While on, the visualisation area shows bars regardless of waveform / heatmap activation. Not persisted.

### Calibration loop: measure, narrow, lock

With the test pattern up, sweep `RENDER_PERIOD_US` and time the beat sweep. Pin the panel's actual scan rate via halving / midpoint refinement, recording each result. Set the chosen `RENDER_PERIOD_US` once the beat is below perception (~10 s sweep period or longer).

### Render rate strategy stays open

60 fps is the current target. Lower rates (e.g. 30 fps with `VIZ_COLS_PER_PUSH` doubled) may produce smoother motion or worse — depends on panel scan interaction. Decision deferred to on-device verdict during the calibration loop.

### Mitigations beyond rate-lock

If rate-lock alone leaves visible tear, follow-up angles include splitting the push into smaller per-frame chunks, decoupling sprite scroll from data updates, or other timing tricks. Out-of-scope unless rate-lock proves insufficient.

## Plan

Explore mode — topics rather than tasks.

**Topics**

- **Test pattern + `Ctrl+T` toggle.** Diagnostic rendering path that replaces audio-derived intensity with a bright-bar-every-30-cols pattern, scrolling with `disp_abs`. Dispatch from `drawVizColIntoSprite`; toggle a `g_viz_test_pattern` flag from the keyboard ctrl branch.

- **Pin actual panel scan rate.** Walk `RENDER_PERIOD_US` through a few candidates (e.g. 16500, 16667, 16800, 16900 µs), time the tear-sweep period at each, narrow to where the sweep crosses zero / reverses. Record values in the Log.

- **Render rate strategy.** Compare smoothness at 60 fps (current) vs 30 fps (period × 2, `VIZ_COLS_PER_PUSH` × 2) under the locked rate. The winner depends on how the panel scan interacts with our push rate — open question, on-device verdict.

- **If rate-lock isn't enough.** Try a fallback (chunked pushes, decoupled scroll-vs-data) only if the calibrated lock still leaves visible tear.

- **Verdict.** Recommendation: locked period value, render rate choice, and whether any fallback is shipping.

**Done when**

The visualisation scrolls without visible tear during normal playback, with the chosen `RENDER_PERIOD_US` and (if relevant) render-rate strategy recorded. Test pattern is left in tree as a future diagnostic if useful; otherwise stripped.

## Log

- Version bumped 0.21.40 → 0.21.41. Build entry: test pattern + Ctrl+T toggle.
- Added `g_viz_test_pattern` flag, `toggleTestPattern()`, and a test-pattern path in `drawVizColIntoSprite` (white bar every 30 cols of audio time, black otherwise). Test pattern can be activated standalone (without waveform / heatmap) — pollVisualisation gate, composeBrowser dispatch, overlay-active key handler all updated to treat the test pattern as an active overlay. `Ctrl+T` toggle wired into both the no-overlay and overlay-active key paths.
- 0.21.42–0.21.50: methodical panel-rate calibration using the test pattern. Bracketed actual scan rate: at 59.5 Hz tear swept L→R over 18 s (very slow, precise), at 60.0 Hz R→L over 1-2 s. Final calibrated period 16780 µs (~59.6 Hz).
- 0.21.47–0.21.50: tried 30 fps × 4 cols/push (panel/2 strategy). Aim was alternating tear/clean frames to halve perceived tear. Result: tear still constantly visible because shift per push (4 px) was bigger than the perception threshold.
- 0.21.51–0.21.52: 3-tap horizontal blur on heatmap and waveform. Modest help — softened gradients reduced tear visibility a bit, didn't eliminate.
- 0.21.53: went to 60 fps × 1 col/push, 4 s on screen. Tear shift drops to **1 pixel**, near-invisible at normal viewing distance. The trade is slower scroll (4 s/screen vs 2 s).
- 0.21.54: tried 1.33 s × 3 cols/push (with SPEC_FFT_SIZE halved to 128 to fit SPEC_COL_SAMPLES = 245). 3-px tear shift — visible compromise.
- 0.21.55: reverted to 4 s × 1 px tear as the chosen default.
- 0.21.56: diagnostics row refresh restored during viz, using a header-rows-only push (`drawHeaderToCanvas` + `presentRows(0, headerH())`) instead of the full-canvas `presentFrame` that was previously suppressed. No measurable contention with the vizTask.
- 0.21.57: blur removed (was a no-op at 1-px shift anyway, cost a little detail). Crisper heatmap / waveform.

## Conclusion

Tear is minimised by two combined moves: render task locked to the empirically-calibrated panel scan period via `esp_timer` (microsecond precision), and per-render shift reduced to 1 column at 4 s on screen. 1-px tear shift is below most perception thresholds — the visualisation looks essentially clean. `Ctrl+T` test pattern stays in tree as a per-device calibration aid; diag row updates again during viz via a lightweight header-rows-only path.

**Tunables worth promoting to Settings (or auto-calibration) in future:**

- **`VIZ_ZOOM_SECONDS`** — time visible on screen. Currently 4 s. 2 s (2 cols/push, 2-px tear) and 1 s (4 cols/push, 4-px tear) are also clean-locked options; fractional zooms aren't unless we add sub-pixel scrolling.
- **`RENDER_PERIOD_US`** — per-device empirical calibration of the panel scan rate (~16780 µs on this Cardputer; expected to vary unit-to-unit with crystal tolerance). Could be auto-calibrated on first boot via a test-pattern measurement.
- **Horizontal blur** — currently disabled. Useful at higher zoom levels (smaller `VIZ_ZOOM_SECONDS`) where the tear shift exceeds 1 px. Could be enabled automatically based on shift size.
- **Test pattern (`Ctrl+T`)** — left in tree as a debug aid. Could be hidden behind a "developer mode" toggle rather than a top-level keybind.

**Documentation impact:** none — implementation detail of the visualisation render pipeline.
