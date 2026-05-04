# Evaluate ESP32-audioI2S as an audio library alternative

## Intent

The internal-RAM optimisation change got HE-AAC M4A playing, but at the edge — `9.8 KB free, 7 KB largest contiguous` during M4A playback, and multi-format track-change has regressed (chains of M4A → MP3 → FLAC → FLAC crash inside Foxen FLAC, two contributing causes including a `setRingDepth` race). The path forward is unclear without picking between a few invasive moves.

One of those moves is replacing `arduino-audio-tools` with `schreibfaul1/ESP32-audioI2S`. The prediction was ~10–15 KB savings on wrapper overhead — not transformative on its own — but the actual number matters before we commit to a multi-day swap or fall back on the canvas drop. The aim is to find out empirically: build a minimal test that plays the same problem M4A through ESP32-audioI2S, measure heap behaviour at the equivalent moments, and compare.

If ESP32-audioI2S gives us enough headroom to make M4A comfortable *and* preserve the canvas, it's the right move. If the savings are as modest as predicted, we have a confirmed answer and can commit to the canvas drop (or another lever) with eyes open.

## Approach

### Shape: a separate, stripped-down build

The eval lives as a second PlatformIO environment (e.g. `eval-audioi2s`) alongside `cardputer-adv`. Its source is a small `main.cpp` in `eval/audioi2s/` that does nothing but: bring up M5Unified for the codec/display, mount the SD, open the reference M4A, drive ESP32-audioI2S to play it, and emit our diagnostic tap. No browser, no UI, no track-change, no diagnostics row.

The current production build is untouched — the eval is comparison data, not a swap.

### Speaker path: bypass M5.Speaker for the test

ESP32-audioI2S writes raw I2S frames itself. M5Unified can configure the ES8311 codec without taking ownership of I2S — we let it bring up the codec and the I2S clock, then hand the I2S peripheral over to ESP32-audioI2S. Loses M5Unified's mixer / ramping / channel queue, but those aren't what we're measuring.

If this can't be made to work cleanly enough to even hear audio, the eval still produces useful memory data — playback isn't the success criterion, the heap profile is.

### Diagnostics: reuse the existing tap

Copy the `[mem.fast]` tap (boot, post_start, ~10 ms intervals during the first 2 s, steady-state) into the eval main. Same instrumentation gives directly comparable readings against the current build's data.

### Reference corpus

Primary: the HE-AAC M4A that's been our reference (`Hostage (Plecta Edit 2020)`). One FLAC and one MP3 as smoke tests to confirm ESP32-audioI2S handles the formats we care about. Headline data is the M4A reading.

### Decision criteria

- **Big win** — M4A steady-state free > 30 KB *and* largest > 20 KB: commit to the swap. Library swap pays for itself.
- **Modest** — improvement under ~10 KB beyond current 9.8 / 7.2: prediction confirmed, swap doesn't pay off, fall back to the canvas drop.
- **No-go** — library doesn't decode HE-AAC, or fails its own way: answer is no, without ambiguity.

### Map

No map edits. The audio nodes in `map.md` are already stale from the v3 migration (still describe ESP8266Audio / `AudioFileSourceM4A`); catch-up is owed by the v3 work, not this eval. The eval is a temporary measurement build that doesn't change the design the map describes.

## Plan

- [x] **Gate: verify HE-AAC SBR support in ESP32-audioI2S** before building anything. Read the library's source / docs / known-limitations issues. If SBR isn't supported, stop the change here and write the Conclusion saying the eval isn't worth running.
- [x] Add `eval-audioi2s` environment to `platformio.ini`. Same board / framework / sdkconfig as `cardputer-adv`; `lib_deps` swaps audio-tools for ESP32-audioI2S; `src_filter` excludes `src/` and includes `eval/audioi2s/`.
- [x] Write `eval/audioi2s/main.cpp`: bring up M5Unified (display + ES8311 codec), mount SD, hard-code the path to the reference M4A, hand I2S to ESP32-audioI2S, drive playback, log the existing `[mem.fast]` tap (boot / post_start / 10 ms taps for first 2 s / steady-state).
- [ ] Build and flash; capture serial output for the reference HE-AAC M4A; record readings in Notes.
- [ ] Smoke-test: swap the hard-coded path to a known FLAC and an MP3, build/flash/capture for each. Confirms ESP32-audioI2S handles our other formats.
- [ ] Compare against the decision criteria in the Approach. Write the outcome (big win / modest / no-go) into Notes.

No version bump. The eval env is not user-visible production firmware — the production `cardputer-adv` build is untouched, and the eval's binary informs a future change rather than shipping. The Conclusion will not propose a `CHANGELOG.md` entry for the same reason.

## Notes

### 2026-04-29 — gate: HE-AAC SBR supported on ESP32-S3

Per the library's README codec table: `aacp` (AAC+ / HE-AAC) on ESP32-S3 / P4 includes `+SBR, +Parametric Stereo`. Base ESP32 is more limited (mono-only), but we're on ESP32-S3 (Cardputer ADV). Gate passes; eval is worth running.

### 2026-04-30 — eval-audioi2s env added, eval main written, build succeeds

`platformio.ini` now has `[env:eval-audioi2s]` (extends `cardputer-adv`, swaps `lib_deps` to ESP32-audioI2S, `build_src_filter` includes only `eval/audioi2s/`). `eval/audioi2s/main.cpp` plays a hard-coded reference file via ESP32-audioI2S after letting `M5.Speaker.begin()` configure the ES8311 codec (then `end()` to release I2S — the disable callback is a no-op so codec stays powered).

Two build-time observations:
- **Static RAM 19.3%** (63 KB) vs production's 21.4% (70 KB) — ~7 KB less in `.bss`. Modest as predicted, but earlier than the on-device readings will land.
- **Flash 60.3%** (2.0 MB) vs production's 41.2% (1.4 MB). Library bundles Opus, Vorbis, libfaad, libhelix-mp3, etc. — broader scope than what we use. Within partition budget (3.2 MB) but worth flagging if we go this way.

ESP32-audioI2S uses **libfaad** for AAC, not libhelix. Confirms the earlier "AAC backend likely a wash" prediction — different library, comparable scale.

One build snag worth recording: ESP32-audioI2S references `CORE_DEBUG_LEVEL` (a standard Arduino-ESP32 build macro). The custom-sdkconfig framework rebuild doesn't emit it, so the build failed until we added `-DCORE_DEBUG_LEVEL=0` to the eval env's flags.

### Resume marker

Next unticked task in the Plan is "Build and flash; capture serial output for the reference HE-AAC M4A". Build is already green — only flash + on-device capture remains. To resume:

```
pio run -e eval-audioi2s -t upload
pio device monitor -e eval-audioi2s
```

Capture from boot through `[mem] post_start` and 5 s of steady-state, paste back, then we have the headline data point against the decision criteria.

