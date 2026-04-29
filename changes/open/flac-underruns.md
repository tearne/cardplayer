# FLAC playback underruns

## Intent

After the v3 migration to `arduino-audio-tools` + `FLACDecoderFoxen`, FLAC playback reaches the speaker but underruns frequently — the audible result is breaks and stutters rather than clean audio. MP3 from the same library plays cleanly under the same conditions. The user wants FLAC playback that holds up without underruns.

The cause is unverified; likely candidates are the Foxen decoder being CPU-heavier than libhelix's MP3 path on this hardware, the audio output's buffer ring being too shallow for FLAC's burstier decode pattern, or both.

## Approach

### Diagnose before fixing

The diagnostics row now shows real per-core CPU load. Watching `cpu 1:` (the audio core) during FLAC playback discriminates the two hypotheses cleanly:

- **Pinned near 100 %** → the Foxen decoder can't keep up with real-time decode on core 1. Headroom is gone; we'd need a faster decoder (or a less ambitious one for FLAC).
- **Comfortably below 100 %** → the decoder is fast enough on average, but its output is bursty (a FLAC frame is 4096–8192 samples, far more than our 1536-sample ring slot can hold) and the ring drains during the gap between bursts. Increasing ring depth gives the decoder room to overrun without underflowing the speaker.

The first task in the Plan is to observe and decide. Subsequent tasks branch on the finding.

### Likely fix: deeper ring buffer

If the diagnosis points at burstiness rather than raw CPU starvation, the smallest workable change is to grow the ring in `audio_output_m5.h`. The current ring is **3 slots × 1536 samples × 2 bytes ≈ 9 KB** (~104 ms at 44.1 kHz mono). Doubling to **6 slots ≈ 18 KB** absorbs a typical FLAC frame's worth of overrun. The cost is modestly more RAM and ~100 ms of additional output latency on play / seek — both acceptable.

### Less-likely fix: faster FLAC decoder

If CPU is the limit, options shrink. Switching to the full libFLAC was attempted during v3 migration and failed because that library pulls in `libogg` which isn't packaged. A workaround would be to ship a minimal `ogg/ogg.h` shim (libFLAC only references it for the Ogg-FLAC encapsulation we don't use) — invasive but possible.

### Map

No map node needs changing; this is implementation-internal tuning of the audio path's buffering or decoder choice.

## Plan

- [x] Diagnose: play a FLAC track and read `cpu 1:` in the diagnostics row during steady-state playback (skip the first second so calibration… well, that's gone now). Note whether the value is pinned near 100 % or comfortably below. The Approach commits to the ring-buffer fix below if the value is below ~85 %; if it's higher, this Plan is wrong and we stop and re-plan.
- [x] In `src/audio_output_m5.h`, increase the ring depth from 3 slots to 6 by changing the buffer's first-dimension constant (and `_ring`'s wraparound modulus). `BUF_SIZE` (1536 samples per slot) stays.
- [x] Verify the new ring buffer's RAM cost stays within budget — 6 × 1536 × 2 bytes ≈ 18 KB, up from 9 KB; should comfortably fit in internal RAM.
- [x] Bump `APP_VERSION` from `0.13.15` to `0.13.16`.
- [ ] On-device test: same FLAC track plays end-to-end without underruns; underrun counter (`u:`) stays at 0 during normal playback; MP3 still plays cleanly (no regression from the ring change); no other regressions.

## Log

- 2026-04-28: User-observed diagnostic confirmed CPU was below pinning during FLAC playback while the `buf` percent oscillated between 0 % and 100 % — classic decoder-burstiness pattern that exhausts a too-shallow ring. Doubled ring depth from 3 to 6 slots (RAM cost 9 KB → 18 KB).
- 2026-04-28: Doubling our pre-decode ring helped (`buf` hits 0 less often) but underruns still occur. Realised the **speaker-side** buffer was the bottleneck — M5Speaker defaults to `dma_buf_len=256 × dma_buf_count=8 ≈ 46 ms` of I2S queue at 44.1 kHz mono. Bumped to `1024 × 16 ≈ 370 ms`, configured before `M5Cardputer.Speaker.begin()`. Bumped to `0.13.17`.
- 2026-04-28: User asked the right question — if the speaker was the issue, why did our ring drain to 0? Looked at `audio_output_m5.h::flush()` and found a wait-for-`isPlaying`-false loop that serialised everything to one chunk in flight at a time, defeating both the deeper speaker DMA queue *and* our deeper pre-decode ring. Removed the wait; `playRaw` is now called back-to-back and the M5 speaker's DMA queue applies back-pressure if it's full. Underruns are now counted only when the queue was empty on entry (the actual underrun condition). Bumped to `0.13.18`.
- 2026-04-28: `0.13.18` was wrong — playback became fast and stuttery. Reading the M5Speaker source: `playRaw(channel=-1)` picks any free channel out of 8, so submitting back-to-back without pinning the channel had multiple chunks playing simultaneously (mixed garbage). Each channel has a built-in 2-deep `wavinfo` queue and `_set_next_wav` blocks internally when full — exactly the back-pressure we want, but only if we pin to a single channel. Updated `flush()` to call `playRaw(..., REPEAT=1, channel=0, stop_current=false)` and dropped the `isPlaying` wait. Bumped to `0.13.19`.
- 2026-04-28: `0.13.19` allocation-failed for 16 KB on first FLAC start. The new speaker DMA size (`1024 × 16 × 2 = 32 KB`) was too big for fragmented internal RAM. Halved to `1024 × 8 ≈ 186 ms`, still ~4× the default. Bumped to `0.13.20`. FLAC played cleanly on a single track.
- 2026-04-28: User testing showed RAM was at ~90 % during FLAC playback, and a track change caused a reboot. Investigation: `ESP.getPsramSize()` returns 0 on this hardware — pioarduino's prebuilt sdkconfig has `CONFIG_SPIRAM` disabled, so the audio-tools `AllocatorExt::ps_malloc` always falls through to `malloc` against internal RAM. `M5Canvas::setPsram(true)` likewise reaches internal RAM. The Cardputer ADV physically has 8 MB Quad PSRAM but it isn't initialised at boot. M4A decoding fails the same way (45 KB MP4 sample-table allocation can't be satisfied).
- 2026-04-28: Tried multiple paths to enable PSRAM. (a) Added `board_build.memory_type = qio_qspi` — no effect alone, sdkconfig flag is required too. (b) Added `custom_sdkconfig = CONFIG_SPIRAM=y` — triggers framework rebuild, fails inside `wpa_supplicant` with macro-redefinition warnings treated as errors. (c) Tried `CONFIG_COMPILER_DISABLE_GCC*_WARNINGS=y` overrides — didn't suppress the failures. (d) Tried `custom_component_remove = wpa_supplicant` — pioarduino's component manager only handles component-manager-managed components, not core ESP-IDF ones, so wpa_supplicant still got built. Reverted all those changes; staying on the working `0.13.20` configuration. Bumped to `0.13.25` to reflect the revert (the failed attempts shipped no working firmware).
- 2026-04-28: This change has met its primary goal — FLAC playback is now clean on a single track (was unplayable before). The track-change-reboot and M4A demuxer-OOM are different problems with the same root cause: internal-RAM pressure with PSRAM unavailable. Stopping per the build-mode rule and proposing to wrap this change here, with the RAM-pressure issue captured in the existing `m4a-he-aac-sbr-oom` change (it's the same root cause; would extend its scope) or in a new change for "PSRAM enablement / RAM pressure".
