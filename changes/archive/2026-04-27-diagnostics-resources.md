# Diagnostics: device resources

## Intent

The diagnostics line currently shows audio task stack, ring-buffer headroom, and underrun count — useful for audio behaviour, but blind to whole-device load. As the firmware grows, the user wants to see how successive changes affect device resources, especially CPU usage per core and RAM usage. Expand the diagnostics line to surface this so the cost of each change is visible at a glance on the device itself.

## Approach

### Row height

The diagnostics row grows from 8 px to 16 px, taking the header from 18 px to 26 px when chrome is not minimised. The browser body shrinks correspondingly. The `g_chrome_minimal` toggle continues to hide the row entirely, so production presentation is unaffected.

### Four sparklines

Four equal-width slots span the row — **stk** (audio task stack peak), **buf** (ring-buffer headroom), **cpu** (per-core load), **ram** (free heap). Each slot carries a small text label across the top (~7 px) and a tiny sparkline along the bottom (~9 px), four cells of ~60 px each across 240 px. Where the current row carried a numeric battery voltage, that readout moves to row 1 of the header next to the battery icon (already the natural place for it).

### Sparkline mechanics

Each sparkline holds a rolling history of samples — one sample per pixel of graph width, so a ~50 px graph is ~50 seconds of history at the 1 Hz sample rate. Each sample is a vertical bar drawn from the bottom of the graph up to a height proportional to the value's fraction of its capacity. New sample appends on the right; the buffer shifts left at each tick. The graph re-renders into the back buffer each second (the rendering work itself rides on whatever back-buffer mechanism `flicker-free-rendering` introduces; this change does not assume that change has landed but plays nicely with it).

### Colour for value magnitude

A 9 px sparkline has poor Y resolution, so colour carries part of the signal. Each bar is coloured by its value: a calm colour below ~60 % of capacity, a warning tint between 60 – 85 %, and a hot tint above 85 %. The thresholds are uniform across all four sparklines so the reader's eye learns one mapping. Concrete colours are an implementation choice for the Plan.

### CPU per core

CPU load is measured per core by hooking the FreeRTOS idle task on each core to increment a counter and sampling the deltas over each 1 s interval — `(elapsed_idle_ticks / total_ticks)` gives the idle fraction; load = 1 − idle. Both cores share one sparkline, drawn as two overlaid coloured lines (one per core) so the relative load of UI vs audio core is visible without doubling the row height.

### Sources for the other metrics

- **stk** — `g_diag_stack_used` already collected; the peak value is the "current" sample at each tick (resetting the peak each tick would lose its purpose, so the sparkline shows the running peak across history).
- **buf** — same source as today (`g_diag_wait_us` ratio), one sample per tick.
- **ram** — `ESP.getFreeHeap()` against `ESP.getHeapSize()`; the bar shows *used*, so a rising line means RAM pressure climbing.

### Underruns

The `u:` underrun counter is repurposed: instead of a numeric readout, the buf sparkline flashes its most recent bar red for one tick whenever the underrun count increments, so the user notices the event without losing any of the four slot widths.

### Terminology

The existing "chrome" naming for the toggle that hides this row is replaced with "diagnostics", which aligns with the existing `drawDiagnosticsRow()` and `DIAGNOSTICS_POLL_MS` and is what the row actually contains. Concretely:

- `g_chrome_minimal` → `g_diagnostics_hidden` (boolean meaning preserved — true means hidden)
- `toggleMinimalChrome()` → `toggleDiagnostics()`
- Help-screen label `chrome` → `diags`
- Stray comment and map-node references updated accordingly.

The keybinding (`` ` ``) is unchanged — only the names move.

## Plan

- [x] Rename: `g_chrome_minimal` → `g_diagnostics_hidden`; `toggleMinimalChrome()` → `toggleDiagnostics()`; help-screen label `chrome` → `diags`; stray code comment that mentions "chrome"; map-node wording for the toggle.
- [x] Bump `HEADER_FULL_H` from 18 to 26 (row 2 grows from 8 to 16); audit and update any layout values that depended on the old header height.
- [x] Move the battery voltage readout from row 2 of the header into row 1, next to the battery icon.
- [x] Add per-core CPU sampling using FreeRTOS run-time stats: verify and enable `configGENERATE_RUN_TIME_STATS` (and any prerequisites) in build flags or `sdkconfig`; each diagnostics tick read the per-core IDLE task's run-time counter, compute load = 1 − (idle_delta / total_delta).
- [x] Add free-heap sampling: `ESP.getFreeHeap()` and `ESP.getHeapSize()` each diagnostics tick; store the used fraction.
- [x] Add a sparkline type: fixed-width rolling buffer of normalised samples ([0, 1]); rendered as bars from the bottom of the cell upward; bar colour driven by the threshold rule (blue-grey < 60 %, yellow 60 – 85 %, red > 85 %).
- [x] Replace the existing `drawDiagnosticsRow()` with a four-cell row — `stk`, `buf`, `cpu`, `ram` — each carrying a small text label across the top and a sparkline below. The CPU cell overlays two coloured lines, one per core. Cells equal-width across 240 px.
- [x] Repurpose underruns: remove the `u:` numeric readout; on each underrun increment, flash the latest `buf` bar red for that tick.
- [x] Bump `APP_VERSION` to the next available minor (0.11.0 unless the `flicker-free-rendering` change ships first, in which case 0.12.0).
- [x] On-device test: row renders at 16 px; all four sparklines populate over time; CPU graph shows two responsive lines (visibly different under fast scroll vs idle); RAM line tracks heap; toggling with `` ` `` hides/shows the row; help screen reads `diags`.

## Log

- Map sentence in the Footer node was updated as part of the rename ("chrome-minimisation toggles" → "diagnostics-row toggle"). Single-word swap, flagged in chat at the time.
- CPU sampling: tried three approaches before settling. (1) Run-time stats via `vTaskGetInfo` — `configUSE_TRACE_FACILITY` is off in the prebuilt FreeRTOS. (2) `ulTaskGetIdleRunTimeCounter()` (with a helper task pinned to core 1 to read the other core's value) — `configGENERATE_RUN_TIME_STATS` is also off, so the symbol isn't exported. (3) Idle-hook **rate counter** with empirical max-rate calibration — works but introduces a calibration period and isn't in real time units. (4, the kept approach) Idle-hook **timestamp accumulator** — each hook call records `esp_timer_get_time()`, gaps between consecutive calls inside the threshold (200 µs) are summed as idle microseconds. Idle fraction = idle_us / wall_us, no calibration. The threshold is set well above the no-preemption gap (sub-µs idle-loop overhead) and well below the typical preemption gap (FreeRTOS tick is 1 ms).
- Diagnostics row redesigned during testing. Initial design (four cells, each label + sparkline) gave four near-static graphs for stk / buf / ram. Switched to: numeric readouts for stk / buf / ram / underrun count in two compact cells, and a wider third cell for `cpu 0:` and `cpu 1:` with their own per-core sparklines stacked vertically using the full row height. Sparkline buffers for stk / buf / ram removed; underrun-flash logic removed (count is now a plain number). Bumped to `0.11.1`.
- Further testing revealed two issues with the threshold-based CPU sampling. (1) Both cores read at 100 % load when idle. The idle task on this build uses WAITI to sleep the CPU between FreeRTOS ticks (1 kHz), so the hook fires at ~1 ms intervals when idle. The 200 µs threshold rejected those gaps, treating them as preemption rather than idle. Bumping the threshold doesn't cleanly fix it because we then can't distinguish "1 ms tick wakeup" from "1 ms preemption". Reverted to the rate-counter approach: idle hook ticks a counter, max rate observed is the "fully-idle" reference, load = 1 − (rate / max_rate). The brief calibration period (each core needs one mostly-idle second) is the cost of working around the lack of run-time-stats APIs in this prebuilt FreeRTOS. (2) CPU-cell layout simplified — both cores share a single full-row-height graph with two overlaid lines (cyan core 0, orange core 1); labels stay stacked on the left of the cell. Spacing tightened (`%3d` → `%d` so numbers butt against the colon). Bumped to `0.11.2`.
- Poll cadence raised from 1 Hz to 4 Hz so the CPU graph is more responsive. Sparkline window correspondingly shrinks from ~52 s to ~13 s (52 px × 250 ms). Other readings refresh faster but the values move slowly, so no visual difference there. Bumped to `0.11.3`.
- CPU labels simplified from per-core ("cpu 0:" / "cpu 1:") to a single "cpu:" prefix on the top row, with both percentages tinted to match their respective lines in the overlaid graph (cyan core 0, orange core 1). Label area shrunk; graph widened to 70 px (~17 s window at 4 Hz). Bumped to `0.11.4`.

## Conclusion

The CPU sampling iterated through four approaches before landing on idle-hook rate counting with self-calibration; the Log walks through why the alternatives didn't survive in this build's prebuilt FreeRTOS. The diagnostics row layout was redesigned mid-build from "four sparkline cells" to "numeric readouts plus a single combined CPU graph." Map catch-up for the Header, Battery, and Diagnostics nodes is pending and will be handled as per-node negotiations.
