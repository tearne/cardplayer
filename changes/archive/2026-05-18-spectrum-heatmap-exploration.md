# Spectrum heatmap exploration

**Mode:** Explore

## Intent

The wider-waveform direction is shelved — RAM budget couldn't carry a deeper pre-buffer, and scroll smoothness turned out to be a bigger problem than the 0.19.9 prototype solved. We still want a richer at-a-glance view of what's playing, and the next direction is a scrolling spectrum heatmap: each column an FFT snapshot, scrolling right-to-left, bin intensity encoded as colour.

The Explore figures out whether this is feasible on the cardputer, what it costs in RAM / CPU / latency, and what the visual model should be (bin count, colour ramp, frequency axis, scroll rate). Smooth scroll stays a hard requirement — to solve, not dodge.

## Approach

### FFT library and size

esp-dsp real FFT, 256 points. Espressif-tuned for the S3, no extra dep. 256 gives ~86 Hz/bin at 44.1k and ~6 ms time resolution; 128 is too few useful bins for visual discrimination, 512 wastes both RAM and time resolution.

### Where the FFT runs

In the audio task, on every 256 samples as they enter the pre-buffer (same tap as the waveform). FFT rate is ~172/sec at 44.1k, independent of display rate. The audio core has headroom post-FLAC; amortising FFT across ingestion fixes the per-frame budget rather than spiking under render. Magnitudes accumulate into the in-progress display column between snapshots — ~7 FFTs averaged per column at the chosen scroll rate, so each column reflects its full ~42 ms slice rather than one arbitrary 5.8 ms FFT inside it.

### Bin-to-pixel mapping

Log-frequency — the 128 useful bins collapse to ~24-32 visible bands via log-spaced grouping. Musical perception is log; linear bins waste vertical space on the sparse high-frequency end. Intensity uses a perceptually-uniform purple → yellow ramp (viridis-style), pre-computed as a small RGB565 LUT.

### Playhead at the right edge, no look-ahead

New columns enter on the right; older columns scroll left into the past. Matches the user's mental model (right-to-left scroll) and drops the look-ahead requirement, so the heatmap doesn't compete with the pre-buffer for RAM the way the waveform did.

### Scroll rate: 10 s across the screen

240 cols × ~42 ms/col = 10 s on-screen. Slow enough to read at a glance, fast enough that a feature crosses in a reasonable time. Tuneable later if it feels wrong on-device.

### Render cadence: 1 px per frame, matching column rate

Render at 24 Hz (≈ column rate), advancing exactly 1 px per frame. No interpolation, no wasted duplicate frames. 1 px/frame is mathematically jitter-free; if it reads as judder rather than smooth motion, the smoothness prototype escalates to sub-pixel rendering or a higher column rate.

### RAM budget fits

~8–10 KB: FFT working buffer + window function (~2 KB), column ring (240 × 28 × 1 byte ≈ 6.7 KB), colour LUT and sample accumulator (~1 KB). The heap pressure that killed the wider waveform won't bite here.

### Smoothness mechanism is for the Plan to prototype

Explore mode — not pre-decided. With 1 px/frame as the starting point, the prototype tests whether that alone is enough; if not, fallbacks (time-driven viewport over a wider column ring, sub-pixel interpolation, display-lag) come in informed by what failed.

### Coexistence with waveform

New overlay alongside the existing waveform, not a replacement. Waveform stays on Ctrl+W; heatmap binds to Ctrl+H. Map-wise it's a sibling of Waveform Visualisation under Browser. The waveform may be revisited later to make it less RAM-/smoothness-challenging, but that's out of scope here.

## Plan

Explore mode — topics rather than tasks.

**Topics**

- **FFT pipeline on the audio core.** esp-dsp wired up, 256-pt real FFT firing once per 256 samples, magnitudes accumulating into the in-progress display column with log-bin grouping to ~24–32 visible bands.

- **Heatmap render path.** Column ring, viridis-style RGB565 LUT, render to the browser slot with new column entering on the right.

- **Toggle and coexistence.** Ctrl+H opens / closes the overlay. Waveform on Ctrl+W still works; both can't show at once. Auto-open setting deferred unless trivial.

- **Smoothness verdict on-device.** Starting at 1 px/frame at ~24 Hz. Evaluate. If it reads as judder, log the symptoms and try one fallback (sub-pixel interpolation or higher column rate) before drawing conclusions.

- **Cost picture.** Measured heap free, per-core CPU load, any new underruns. Numbers captured in the Log against baseline.

**Done when**

We have a written recommendation: is the heatmap worth shipping (and at what scroll rate / smoothness mechanism), does it need more work to be acceptable, or does the prototype say it doesn't pay off. Smoothness is judged subjectively on-device by the user — no objective metric.

## Log

- Version bumped 0.19.10 → 0.20.0 (minor, approved at Build entry). The 0.19.9-era wider waveform prototype remains in tree alongside the new code; not reverted.
- Added esp-dsp lib_dep (Espressif fork on GitHub). Carries some build risk — unproven with this pioarduino platform pin. First flash will tell.
- Spectrum tap wired into `audio_output_m5.h`: 256-pt real FFT every 256 samples, Hann window, log-grouped into 28 visible bands, column committed every ~1850 samples (~42 ms). 240-col ring, no widening (the cinema-frame approach doesn't need it).
- Heatmap render path in `main.cpp`: `composeHeatmapView` renders 240×28 1-px rects via inline viridis interpolation. Mutually exclusive with the waveform overlay. `Ctrl+H` toggles; existing dismiss-on-any-key behaviour extended to cover both overlays. Search-takeover dismisses both.
- Render uses 16 ms (~60 Hz) cap but skips frames where `spectrumRing().abs_head` hasn't moved — gives the intended 1-px-per-column-write motion regardless of UI tick.
- Ready for first flash. Watch for: build errors from esp-dsp inclusion, RAM impact (expect ~10 KB new `.bss`), spectrum colours reading right under playback.
- 0.20.0 build failed: esp-dsp's GitHub repo includes example apps (`azure_board_apps`, etc.) that PlatformIO tries to compile, hitting missing `ssd1306.h` / `mpu6050.h`. Swapped to `kosme/arduinoFFT@^2.0.4` — PIO-native, no demo-app baggage. FFT logic rewritten to use the ArduinoFFT class. Bumped to 0.20.1 for retry.
- 0.20.1 flashed and renders. Heatmap visible on Ctrl+H, colours alive. Reads as "swamp of yellow" though — per-column max-pooling over 10 s/screen smooths transients into sustained bands.
- 0.20.2: dropped scroll period from 42 → 21 ms/col (10 s → 5 s on screen) for less averaging per column.
- 0.20.2 still read as "very solid yellow" — speed felt right, dynamic range did not. Root cause: dB reference was at `mag=1`, but real FFT magnitudes routinely hit 10–100, saturating the LUT at yellow.
- 0.20.3: 0 dB reference moved to `SPEC_FFT_SIZE/2 = 128` (full-scale single bin). Floor widened from −60 dB to −80 dB.
- 0.20.3 read better but the bottom band sat at a near-constant colour through music — music's natural low-end-heavy spectrum was the cause, not a bug. 0.20.4: +3 dB/oct frequency-tilt (multiply magnitudes by `sqrt(k)`) to flatten the average response across bands.
- 0.20.5: bumped tilt to +6 dB/oct (multiply by `k`).
- 0.20.6: faster scroll — column period 21 → ~10 ms (5 s → 2.5 s on screen). 1.8 FFTs averaged per column. Hard floor noted in source: must stay ≥ `SPEC_FFT_SIZE` to avoid empty columns.
- 0.20.7: heatmap render filled only `(inner_h / SPEC_BINS) * SPEC_BINS` pixels, leaving leftover black strips above and below — visible when diagnostics row was open (and the browser area was smaller). Rewrote the bin Y positions to tile `inner_h` exactly.
- 0.20.8: smoothness mechanism. Replaced fixed 16 ms tick + abs_head-driven gate with a wall-clock-paced cursor (`g_heatmap_disp_abs`). Period derived from column rate so renders fire at exactly 1 px/frame in the common case; floored at 8 ms (125 Hz). Bursty FFT writes no longer translate into bursty display motion.
- 0.20.9: slight tearing reported. `presentRows` uses DMA-backed `pushSprite` (non-blocking); back-to-back pushes at column rate could race the panel still reading canvas. Added `waitDisplay()` after every push. Separately noticed: the canvas is 8 bpp RGB332, so the 256-entry viridis LUT collapses to ~256 distinct on-canvas values (only 4 blue levels). Worth a follow-up if colour banding matters.
- 0.20.9 made the scroll much slower and exposed a visible left-to-right panel scan during each render — `waitDisplay()` blocks for the panel's full refresh (~16 ms+), well over the 10 ms column period. Reverted in 0.20.10. Tearing mitigation now needs a lighter-weight sync (DMA-only) or a different angle entirely.
- 0.20.10 was reverted in error — 0.20.9 actually read as "lovely and smooth", just with a visible left-to-right scan line. Slower render rate (forced by `waitDisplay`) is fine; the scan-line artifact is the remaining problem. 0.20.11 restores `waitDisplay()` and scopes the scan line as the next thing to chase.
- 0.20.12: pollHeatmap pushes only the inner heatmap rect (skipping the 1-px frame lines top and bottom). Marginal — saves ~2 of 95 rows — but a cheap first try at narrowing the DMA window.
- 0.20.12: scan-line artifact diagnosed — `waitDisplay()` caps render at ~60 Hz, column rate is ~96 Hz, so display lags audio at ~36 cols/sec. After ~6 s the lag exceeds the 240-col ring and the displayed window starts reading slots being actively written, manifesting as a left-to-right wrap. The `step = 1` advance was the root cause.
- 0.20.13: `disp_abs` is now a `double`, advanced by `elapsed_us / col_period_us` each poll. Average advance rate matches audio rate regardless of how often renders fire. Render-rate caps now translate into 1-or-2-pixel steps per frame instead of accumulating lag. Slight texture vs perfect smoothness, but no wrap-around.
- 0.20.14: slower scroll — column period back to ~21 ms (`SPEC_COL_SAMPLES = 920`), 5 s on screen. Render rate (~48 Hz) now sits comfortably under the 60 Hz `waitDisplay` ceiling, so 1-px steps dominate.
- 0.20.15: experiment with chunked rendering. New constant `HEATMAP_COLS_PER_PUSH` controls how many cols of `disp_abs` accumulate between pushes. Started at 2 (push every other column). Trades scroll smoothness for fewer push events / less tearing exposure.
- 0.20.16: `HEATMAP_COLS_PER_PUSH = 4` for comparison.
- 0.20.17: settling on 3 s on screen (`SPEC_COL_SAMPLES = 552`, ~12.5 ms/col) and `HEATMAP_COLS_PER_PUSH = 2`.
- 0.20.18: two improvements driven by the "FFT tap leads audible playback by ~220 ms" discussion. (a) `HEATMAP_LOOKAHEAD_MS = 110` — disp_abs target is now `abs_head - lookahead_cols`, partial compensation so the rightmost column is closer to audible-now without dropping all the look-ahead. (b) `hardFlush` now wipes the spectrum ring, and `stopPlayback` re-anchors `disp_abs`, so a track skip blanks the heatmap instead of carrying old audio's bins forward. Pause needs no change — abs simply stops advancing, display naturally freezes.
- 0.20.19: 0.20.18 cleared the ring but didn't force a render, so the cleared state only reached the panel after the next pollHeatmap tick — by which point the new track had already started writing. `stopPlayback` now forces a redraw when the heatmap is active; pollHeatmap's re-anchor branch also renders instead of just setting state.
- 0.20.19 didn't build — `stopPlayback` calls `composeBrowser` but the forward decl was further down the file. 0.20.20 adds an earlier forward decl.
- 0.20.20 still didn't clear on intra-track jumps (`1`-`0` and `[`/`]` go through `seekToByte`, not `stopPlayback`). Added a separate `resetSpectrum()` method (no speaker stop, no pre-buffer flush) and called it from `seekToByte`, with the same heatmap re-anchor + force-redraw. 0.20.21.
- 0.20.21 cleared correctly but the new "wave front" still showed pre-jump audio, then scrolled left and stayed visible. Root cause: the FFT tap is at decoder ingress, and the decoder has its own internal sample buffer that drains pre-seek audio through `ConsumeSample` after the seek. 0.20.22: `resetSpectrum` now also suppresses FFT updates for ~200 ms (`sampleRate() / 5` samples) so decoder residue passes through without seeding the spectrum.
- 0.20.22 broke under heavy held-seek (no playback, CPU1 100 %, watchdog reset). Likely cause: every seek triggered a forced render in `seekToByte` AND another in the following `pollHeatmap` (the `prev_us == 0` re-anchor branch added in 0.20.19) — ~60 ms of render work per 100 ms of held seek, plus an unlocked read of the ring during the forced render. 0.20.23 removes the force-renders from `seekToByte` and `stopPlayback`; pollHeatmap's re-anchor path alone handles the redraw, picked up within ms by the main loop's tick. Halves the per-seek render cost.
- 0.20.24: temporarily setting `HEATMAP_LOOKAHEAD_MS = 0` to isolate whether the lookahead math is contributing to the "samples split across left and right edges" artifact. Pure diagnostic, will be restored once the visual issue is understood.
- 0.20.24 reduced the artifact to a few pixels, confirming the lookahead was contributing. Root cause for the residual: `SPEC_COLS = SCREEN_W = 240` left no headroom between writer and reader — a 1-col lag in `disp_abs` made `(abs-241) % 240 == (abs-1) % 240`, wrapping the latest-written slot into x=0. 0.20.25: `SPEC_COLS = 480` (240 cols of margin), `HEATMAP_LOOKAHEAD_MS = 110` restored. Cost: ~6.7 KB extra RAM for the ring.
- 0.20.26: pause sent CPU1 to 100 %. Cause: `pollHeatmap` re-rendered every ~25 ms regardless of whether the displayed window had moved. Added a `last_rendered_abs` check that skips the compose+push when `disp_abs` (truncated) is unchanged since the previous render.

## Conclusion

The heatmap ships. Smoothness solved by a wider-than-screen ring (480 cols, 2 × screen) plus a wall-clock-paced fractional `disp_abs` cursor — the waveform's failed 0.19.9 viewport approach was correct in shape, just needed the ring headroom so writer and reader operate on disjoint slots. +6 dB/oct frequency tilt, viridis LUT, ~110 ms partial lookahead compensation, `waitDisplay`-synced presents.

Deferred to follow-up changes: the 0.19.9 wider-waveform prototype code is still in tree unused; the canvas's 8 bpp RGB332 quantises the viridis LUT to ~256 visible values; further user-requested tweaks parked for next time.

**Documentation impact:** new Heatmap Visualisation node belongs as a sibling of Waveform Visualisation under Browser; map will be caught up via per-node negotiation post-archive.
