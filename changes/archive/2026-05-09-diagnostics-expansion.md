# Diagnostics expansion

**Mode:** Explore

## Intent

The diagnostics row is cramped — four numeric readouts plus a two-core CPU sparkline in the ~16 px header strip. Hard to read at that height, and no headroom for additional stats worth surfacing during development.

Explore a bigger diagnostics area: a larger CPU graph plus whatever additional readouts the framework actually exposes that would aid development.

End state: a recorded decision on the new shape with a clear next step.

## Approach

### Scope: the diagnostics area's size and contents

What's mutable — the height of the diagnostics row (currently fixed at ~16 px), the layout of its readouts, the set of stats shown. The hide-via `` ` `` toggle stays. The browser area shrinks correspondingly when the row grows.

### Method: inventory before layout

Survey what stats the framework actually exposes cheaply (ESP heap caps APIs, FreeRTOS run-time stats, audio internals already in the codebase) before deciding what to render. Designing layouts first risks committing to readouts the platform won't give us cheaply.

### Constraint: audio core unaffected

New readouts must read without contending with audio decode — same rule the existing CPU sampling follows.

### Decision criteria

The new shape ships if (1) the bigger CPU graph is materially more readable than the current 16 px one, (2) any added stats earn their pixels rather than fill them, and (3) the browser-area reduction is acceptable or the expansion is opt-in.

## Plan

**Topics**

- Available stats inventory: what the framework exposes cheaply that's worth surfacing — ESP heap caps APIs, FreeRTOS run-time stats per task, audio internals already in the codebase, plus anything else surfaced during the read.
- CPU graph readability at larger sizes: how much height it takes to make both cores comfortably distinguishable; whether overlaid (today's shape) or stacked / split.
- Layout exploration: 1–3 candidate diagnostics-area sizes filled with the chosen stats; trade-off in browser-area pixels for each.
- Toggle behaviour: cycle (hidden → normal → expanded) vs two-state (hidden ↔ expanded), informed by what the layout work uncovers about whether a "small footprint" mode is still useful.

**Done when**: a Recommendation is written into this change with the chosen diagnostics shape (size, stats list, toggle behaviour) and a clear next step — folded-in implementation tasks added to this Plan if small, or a follow-up change opened with the recommendation as its Intent.

### Implementation tasks (folded in)

- [x] Update header constants — `HEADER_ROW2_H` 16 → 32, recompute downstream layout (browser top, sparkline width)
- [x] Sample heap largest-free-block and minimum-ever-free in `pollDiagnostics()`; add globals + map them onto `L` / `M` numeric readouts
- [x] Rewrite `drawDiagnosticsRow()` to the new two-sub-row layout: numerics on top, full-width CPU sparkline below
- [x] Resize sparkline buffer (`Sparkline::samples`) and graph constants for full-screen width
- [x] Bump `APP_VERSION` from `0.16.1` to `0.16.2`
- [x] On-device verification: numerics row reads stk/buf/ram/u/L/M/cpu0/cpu1 without overlap; CPU sparkline visibly more readable; `` ` `` toggle still cycles hidden ↔ expanded; browser fits ~2 fewer entries when expanded; no regressions in playback or other UI

## Log

- 2026-05-09: First on-device pass put numerics on two horizontal sub-rows above a full-screen-width graph at 16 px height. User asked to maximise graph y-resolution. Re-arranged: numerics moved to a 4×2 grid in a left column (90 px wide), graph fills the right portion at the full 32 px row-2 height (2× y-resolution). Trade: graph width 240→148 px, history ~60→37 s. Net layout matches the original ask better — readability of the graph was the primary motivation.

## Conclusion

Shipped. Diagnostics row now 32 px tall (header total 26→42 px), with the CPU sparkline taking the full row height for 2× y-resolution. Two new heap readouts (`L`, `M`) cover fragmentation and peak pressure. Toggle stays two-state. Browser loses ~2 entries when shown; the hidden default keeps that out of the user's way.

Final shipped version: `0.16.2`.

### Proposed `CHANGELOG.md` entry

```
## 0.16.2 — 2026-05-09

- Diagnostics row (toggle `` ` ``) gets a taller CPU graph and two new heap readouts: `L` (largest free contiguous block) and `M` (minimum-ever-free, peak pressure mark). Browser loses ~2 entries when the row is shown.
```

### Map node starter draft (post-archive Diagnostics negotiation)

To replace the existing Diagnostics node body:

> Second row of the header, showing live device-resource readouts useful during development. Hidden by default; toggle with `` ` ``.
>
> **Detail**
>
> - Eight numeric readouts in a 4×2 grid plus a per-core CPU sparkline, refreshed four times a second. The sparkline takes the full row-2 height (32 px) for y-axis resolution.
>
> - **stk** — peak audio-task stack use, as a percentage of its 8 KB allocation. Watermark — never decreases.
>
> - **buf** — ring-buffer headroom: time spent waiting for the speaker to drain before submitting the next chunk, as a percentage of a 2 ms cap.
>
> - **ram** — internal-heap use as a percentage of total.
>
> - **u** — cumulative count of ring-buffer underruns since boot.
>
> - **L** — largest contiguous free block in internal heap, as a percentage of total. Fragmentation signal alongside `ram`.
>
> - **M** — minimum-ever-free internal heap since boot, expressed as percent used at the worst point. Peak-pressure mark — monotonic, only ever rises.
>
> - **c0 / c1** — per-core load as a percentage; the sparkline (cyan core 0, orange core 1) holds ~37 s of history.

## Findings

### Stats inventory

**Heap (`heap_caps_*`)** — all four sub-APIs available cheaply:

- `get_free_size(INTERNAL)` — total free. Already shown as `ram` percent.
- `get_largest_free_block(INTERNAL)` — largest contiguous chunk. Not shown. **Useful: fragmentation indicator** — `ram` may say 50 % free but if the largest block is 2 KB the next 50 KB allocation fails.
- `get_minimum_free_size(INTERNAL)` — low-water mark since boot. Not shown. **Useful: "did we ever come close to OOM?"** without having to watch a graph.
- `get_total_size(INTERNAL)` — denominator (~341 KB).

**FreeRTOS introspection** — all three relevant sdkconfig flags enabled (`USE_TRACE_FACILITY`, `VTASKLIST_INCLUDE_COREID`, `GENERATE_RUN_TIME_STATS`):

- `vTaskGetInfo(handle, ...)` — per-task: `ulRunTimeCounter`, `usStackHighWaterMark`, state, priority, core. Currently used on IDLE0/IDLE1 for the CPU-load graph.
- `uxTaskGetSystemState(...)` — bulk task enumeration. Could surface **per-task CPU%** (audio task vs loop task vs IDLE rather than just per-core).
- `uxTaskGetNumberOfTasks()` — count. Trivia.

**Audio internals** — already exposed via the `g_diag_*` globals: stack peak (`stk`), buffer headroom (`buf`), underrun counter (`u`).

**Hardware/system** — `temperatureRead()` returns die temperature in Celsius. **Useful: thermal indicator** — catches "device is throttling under sustained decode" at a glance. ~1 line of code, no contention.

**Already on the header** — battery voltage, level, version/breadcrumb. Not duplicating those.

**Not useful** — `ESP.getCpuFreqMHz()` (static), flash chip info (static), USB-CDC connection state (visible from cable), Wi-Fi state (unused).

### Decided additions

User picked **(1) heap largest-free-block** and **(2) heap minimum-ever-free** — fragmentation signal and peak-pressure indicator. Die temperature and per-task CPU% dropped: not worth the pixels. The per-core graph stays (no need to swap for per-task).

### CPU graph readability

Current graph: 70 px wide × ~16 px tall, two-core overlaid sparkline (cyan core 0, orange core 1), 4 Hz poll, ~17 s history. Vertical resolution at 16 px is coarse — 0–100 % maps to 16 distinct levels, so 6 % is the smallest visible step. Both cores' lines often overlap.

Doubling height to ~32 px gives 32 levels (3 % step) — much smoother and easier to disambiguate the two lines visually. Going wider too — full screen width (240 px instead of 70) — extends history from ~17 s to ~60 s, useful for spotting sustained load patterns through a track.

### Layout

Available constraints: 240 × 135 display. Footer takes 10 px at bottom. Header currently 26 px; row 1 (10 px) carries version/breadcrumb + battery, row 2 (16 px) carries diagnostics. Browser fills the gap (~98 px today).

Three plausible expansions of the diagnostics row:

- **Modest** — row 2 grows 16 → 24 px (header → 34 px). CPU graph becomes 24 px tall but stays narrow (~70 px) so numerics fit alongside. Two new readouts (`L`, `M`) added to the existing line. Browser loses 8 px (~1 entry).

- **Substantial** — row 2 grows 16 → 32 px (header → 42 px). Numerics go on top (stk / buf / ram / u / L / M, possibly compact labels), full-width CPU graph below at ~22 px tall. Most readable, biggest information gain. Browser loses 16 px (~2 entries).

- **Big** — row 2 grows 16 → 48 px or more (header → 58 px+). Three sub-rows: numerics, per-core stacked sparklines, more. Hits diminishing returns — at 48 px the per-core overlap problem doesn't get noticeably better than 32 px stacked.

The Substantial option (32 px row, full-width graph below numerics) feels like the sweet spot — meaningful CPU-graph improvement, comfortable room for the two new numerics, and the browser's two-entry loss is recoverable by hiding diagnostics with `` ` `` when not actively debugging.

**Picked: Substantial.**

### Toggle behaviour

With Substantial picked, the gap between hidden (10 px header) and shown (42 px header) is 32 px — visible. The Approach flagged the question of whether the toggle should cycle three states (hidden → minimal → expanded) or stay two-state (hidden ↔ expanded).

Two-state wins:

- The diagnostics-row default is already hidden (since `0.16.0`), so most users never see the row at all. The "minimal" intermediate is dev-only, not a comfort layer for end users.
- A dev opening diagnostics is opening them to debug — they want the full picture, not a half-shown row that hides part of what they came to see.
- 2-state matches the existing keypress cadence (single `` ` `` toggle = on/off). 3-state introduces "where am I in the cycle" cognitive load.

The small-footprint mode dies, but it wasn't earning its keep — the browser's headroom is already preserved by the hidden default.

## Recommendation

**Shape**:

- Diagnostics row grows from 16 px to 32 px (header total: 26 → 42 px; browser loses 16 px / ~2 entries when shown).
- Top sub-row of the diag block: numerics — existing `stk`, `buf`, `ram`, `u`, plus new `L` (largest free block) and `M` (minimum-ever-free), plus per-core CPU% (`cpu 0`, `cpu 1`). Compact labels where needed to fit.
- Bottom sub-row: full-screen-width CPU sparkline (~22 px tall, ~60 s history at the existing 4 Hz poll), two-core overlaid as today (cyan core 0, orange core 1).
- Toggle: stays two-state (`` ` `` cycles hidden ↔ expanded). The small-footprint intermediate is dropped.

Implementation is small enough to fold into this change rather than open a follow-up — constants update, two new sampled values, sparkline buffer-size change, redraw logic for the new layout.

## Next step

Folding implementation tasks into this Plan:
