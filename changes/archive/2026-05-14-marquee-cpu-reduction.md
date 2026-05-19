# Marquee CPU reduction

**Mode:** Formal

## Intent

The scrolling track name in the footer currently takes roughly 25% of the UI core's CPU. Reduce that to free duty-cycle for automatic light-sleep, with the expectation of ~15–20% longer battery runtime during playback. Visual tweaks (slower scroll speed, different pause timings, partial redraws that look slightly different) are acceptable in service of the CPU saving.

## Approach

### The two costs

Each marquee tick today: redraw the name region on the off-screen canvas, then call `presentFrame()` which pushes the **whole** 240×135 canvas to the panel — ~65 KB over SPI per tick. At 20 Hz that's ~1.3 MB/s of SPI traffic for the marquee alone, plus the per-tick text re-render. The two costs to cut are SPI traffic and tick frequency.

### Halve the tick rate

Drop `MARQUEE_TICK_MS` from 50 ms (20 Hz) to 100 ms (10 Hz). Keep the per-tick step at 1 px, so the effective scroll speed halves (20 → 10 px/s). The user has signed off on small visual changes; a slower scroll is the trade-off.

Reason: tick frequency is the multiplier. Halving it halves the per-second cost of everything else.

### Partial-row push instead of full-frame push

Replace `presentFrame()` (240 × 135 px = ~65 KB) in `pollMarquee` and the equivalent path in `playback-progress`-redraw paths the marquee triggers, with `presentRows(footerY(), footerH())` — which pushes only the ~10 footer rows (~5 KB). The footer is the only region that changes per marquee tick; the rest of the canvas is unchanged so re-pushing it is wasted SPI.

Reason: this is the dominant cost, ~13× reduction.

### Skip text re-render when the offset hasn't moved

Already done — `pollMarquee` only calls `drawSlotName` + push when `offset_px` actually changed. No change needed; just noting it survives.

### Optional: pre-rendered text sprite

After the two main wins land, measure. If CPU is already in the 1–3 % range, stop. If meaningful headroom remains, take a third step: on track change, render the full track-name text into a side `M5Canvas` of size (text_width × footer_height). Per tick, blit a strip from that sprite into the main canvas at the right offset rather than re-rasterising glyphs. Costs ~2–6 KB of RAM per active track name. Sequenced after measurement so the decision is informed.

## Plan

### Tasks

- [x] Halve marquee tick rate — `MARQUEE_TICK_MS` 50 → 100, scroll speed halves
- [x] Per-tick partial push — `presentRows(footerY(), footerH())` replaces `presentFrame()` on the marquee path
- [x] Measure: read core-1 CPU during playback of a track with a long name and record before/after
- [x] Decide on the sprite step from the measurement — user opted to try it anyway for empirical data
- [x] Pre-render text to side sprite + blit strip per tick — *measured: no significant CPU difference; reverted*

## Log

- 0.17.9: tick rate halved (50 → 100 ms) and the two `presentFrame()` calls on the marquee path replaced with `presentRows(footerY(), footerH())`. Built clean. Handing back for the CPU measurement.
- Measured: core 1 dropped from ~25 % to ~6 % with the marquee scrolling. 4× reduction.
- 0.17.10: sprite step added on user request for empirical comparison. Track name is now rasterised once into an off-screen `M5Canvas` when the playing track changes; `drawSlotName` blits a slice of that sprite into the main canvas at the marquee offset rather than re-rendering glyphs from the font each tick. Sprite is cache-keyed on `g_play_path` so any path mismatch triggers a rebuild. RAM cost: width × footer-height × 2 B (typically 3–8 KB for real track names).
- 0.17.11: sprite reverted. Measurement confirmed no significant CPU improvement vs 0.17.9, matching the upfront estimate (text rasterisation was already a tiny fraction of per-tick cost). Code restored to the simpler direct-render shape; the few KB of RAM goes back to the heap.

## Conclusion

Two changes landed: tick rate 50 → 100 ms, and the two `presentFrame()` calls in `pollMarquee` replaced with `presentRows(footerY(), footerH())`. Core 1 load while scrolling fell from ~25 % to ~6 %.

The sprite-cache step was implemented and measured for empirical data, then reverted — it produced no significant CPU improvement, confirming the upfront estimate that text rasterisation was a tiny fraction of per-tick cost.

**Changelog entry:**

> Track-name marquee scroll now ~4× cheaper. Tick rate halved (so the scroll moves at half speed), and per-tick redraws push only the footer rows to the panel instead of the whole canvas. Core 1 load during playback dropped from ~25 % to ~6 % — frees duty-cycle for automatic light-sleep, expected to extend battery runtime meaningfully.
