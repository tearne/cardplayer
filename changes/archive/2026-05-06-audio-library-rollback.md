# Audio library rollback to ESP8266Audio

**Mode:** Formal

## Intent

The v3 migration (v0.13.15, archived 2026-04-28) coupled two moves into one shipping change: Arduino-ESP32 v3 for calibration-free CPU stats, and a swap of the audio library from `earlephilhower/ESP8266Audio` to `pschatzmann/arduino-audio-tools`. The platform half delivered cleanly. The audio-library half left two regressions still live in `0.13.30` — FLAC playback underruns frequently (Foxen FLAC's burstier output, tracked in `changes/open/flac-underruns.md`), and chains of multi-format track-changes crash inside Foxen FLAC. Pre-migration, both these paths worked.

This change rolls back the audio library only. The platform stays on Arduino-ESP32 v3 with the run-time-stats plumbing. The in-house M4A demuxer comes back. ESP8266Audio's HTTP/ICY streaming files — the one piece that doesn't compile under pioarduino — are excluded via a small pre-build script (the lighter alternative the v3-migration plan flagged but didn't take).

HE-AAC SBR M4A remains unsupported either way: that's a libhelix decoder-level memory appetite (~50 KB contiguous) the wrapper choice doesn't affect. Users with iTunes Store HE-AAC downloads will need to re-encode to LC-AAC or MP3.

The Audio Formats node in `map.md` already describes the rollback target (lines 169–178 still say ESP8266Audio + in-house `AudioFileSourceM4A`) — a stale-catch-up the v3 migration's Conclusion flagged as owed but never landed. The code is the thing out of step, not the map.

## Approach

### Roll back the library only, keep the platform

Swap `pschatzmann/arduino-audio-tools` for `earlephilhower/ESP8266Audio` `^1.9.9` — latest within the 1.9.x line the pre-migration code was written against. Keep pioarduino 55.03.38, the sdkconfig overrides, the run-time-stats CPU sampling, and `patch_log_wrapper.py`. The two regressions are pipeline-specific; the platform half of v3 is doing its job.

ESP8266Audio `2.4.1` exists (Oct 2025, major-version bump from 1.x) and is worth a separate follow-up change once this rollback ships and stabilises — bounded in its own Approach with API-compatibility eval as a Plan task.

### Restore the in-house M4A demuxer from `a65ed50^`

Re-deriving risks re-inventing parts the original got right (AAC-profile filter, chunk-index builder, stsz sliding window). The original shipped working from v0.7.0 onward.

### Remove HTTP/ICY files via a pre-build script

ESP8266Audio's only files failing to compile under pioarduino are `AudioFileSource{HTTPStream,ICYStream}.{cpp,h}` — webradio code with no current or planned use. A script modeled on `patch_log_wrapper.py` overwrites them with an empty marker, idempotent across framework reinstalls — lets us pin a stock release rather than fork.

### Ring depth back to 3 slots; dynamic resize drops

The 6-slot default was a Foxen-FLAC workaround (`flac-underruns`); ESP8266Audio's libFLAC shipped cleanly at 3 slots pre-migration. Reverting reclaims ~9 KB internal RAM. The per-track `setRingDepth()` API drops too — the dynamic resize was only used for the HE-AAC SBR workaround, which leaves with HE-AAC out of scope. If libFLAC turns out burstier than remembered, on-device acceptance catches it via the underrun counter and the ring grows back as a follow-up.

### Re-base `audio_output_m5.h` on ESP8266Audio's `AudioOutput`

`SetChannels/Bits/Rate` virtuals plus `ConsumeSample(int16_t[2])` replace the audio-tools `setAudioInfo` plus `write(uint8_t*, size_t)`. The triple-buffer ring, the channel-pinned `playRaw()`, the underrun and `_last_wait_us` diagnostics, and the `setRingDepth()` API all stay — those are speaker-integration properties, not library properties.

### Archive `flac-underruns` alongside this change

Foxen FLAC goes away with the rollback, taking the underrun cause with it. The change's verification step would test a decoder no longer present; closing it as superseded.

## Plan

- [x] Restore `src/AudioFileSourceM4A.{cpp,h}` from `a65ed50^`
- [x] Add `scripts/exclude_esp8266audio_http.py` (overwrites `AudioFileSource{HTTPStream,ICYStream}.{cpp,h}` in the package dir with an empty marker, idempotent across framework reinstalls)
- [x] Update `platformio.ini`: swap `lib_deps` to `earlephilhower/ESP8266Audio @ ^1.9.9`, drop `arduino-audio-tools` / `arduino-libhelix` / `arduino-libfoxenflac`, register the new pre-build script in `extra_scripts`
- [x] Re-base `src/audio_output_m5.h` on ESP8266Audio's `AudioOutput`; ring depth fixed at 3 slots, `setRingDepth()` API removed
- [x] Rewrite the audio path in `src/main.cpp` for ESP8266Audio: per-extension `makeGenerator()`, `AudioFileSourceSD` wrapped in `AudioFileSourceM4A` for `.m4a` / `.mp4`, audio-task body calls `gen->loop()` with EOF setting `g_advance_pending`
- [x] Delete `src/m4a_stsz_buffer.h` if unreferenced by the restored demuxer
- [x] First clean build green; iterate compile/link errors
- [x] On-device acceptance: WAV / MP3 / FLAC / AAC / M4A-LC-AAC each play cleanly end-to-end; underrun counter stays at 0 during FLAC; multi-format track-change (M4A → MP3 → FLAC → FLAC) does not crash; pause / seek / skip / volume / browser / battery / diagnostics row behave as `0.13.30`
- [ ] Extended on-device testing: longer sessions across the formats actually on the SD card, multi-track auto-advance through real folders, sustained playback to confirm the once-per-second `[mem] steady` reading stays stable, and any reproduction attempts of the pre-rollback FLAC underruns and multi-format track-change crashes
- [x] Bump `APP_VERSION` from `0.13.30` to `0.14.0`
- [x] Archive `changes/open/flac-underruns.md` to `changes/archive/2026-05-06-flac-underruns.md` with a Log entry noting Foxen FLAC removal superseded the in-progress fix

## Log

- 2026-05-06: First clean build green at `0.14.0`. Static-footprint reduction substantially larger than expected: RAM `21.4%` → `11.4%` (about −33 KB of `.bss`/`.data` internal RAM) and flash `41.2%` → `26.1%` (about −540 KB). The modular pipeline, three decoder backends, and Foxen FLAC together were carrying significant static weight that the simpler ESP8266Audio path doesn't impose. Plus the 9 KB ring saving on top. Awaiting on-device acceptance.
- 2026-05-06: Starting version when the Plan was drafted was `0.13.33`, not `0.13.30` as the Plan task said — three patch bumps had landed between earlier conversation context and execution. Bump destination `0.14.0` is unchanged.
- 2026-05-06: Removed two stray references in `setup()` that the Plan didn't explicitly call out: `AudioToolsLogger.begin(...)` (no longer in scope) was deleted; `g_canvas.setPsram(true)` left in place — it's a no-op on PSRAM-less hardware (the canvas falls back to internal RAM, which is what's currently happening) and removing it is a separate cleanup unrelated to the audio rollback.
- 2026-05-06: First flash boot-looped on `i2s(legacy): CONFLICT! The new i2s driver can't work along with the legacy i2s driver`. addr2line traced the abort to `check_i2s_driver_conflict` in `i2s_legacy.c:1999` called from `do_global_ctors` — a C++ static constructor was tripping the conflict before `setup()` ran. Cause: ESP8266Audio's `AudioOutputI2S.cpp`, `AudioOutputI2SNoDAC.cpp`, and `AudioOutputSPDIF.cpp` include `<driver/i2s.h>` (legacy); M5Unified includes `<driver/i2s_std.h>` (new, ESP-IDF v5). Even though our code never instantiates those backends, linking both subsystems registered both drivers at startup and ESP-IDF v5's conflict-check aborted. Fix: extended `scripts/exclude_esp8266audio_http.py` to also empty out the three I2S/SPDIF backends. Renamed in spirit but not on disk — the script's role is now broader than HTTP/ICY exclusion. The audio path goes through `audio_output_m5.h` (M5.Speaker.playRaw) so none of these backends are needed.
- 2026-05-06: First on-device M4A playback exceeds the Approach's prediction. The eval's reference file (`Hostage (Plecta Edit 2020) [YaSXaptWg04]-Billie Eilish.m4a`) plays — expected outcome was rejection at parse (in-house demuxer's AOT 1–4 filter, vs. HE-AAC's AOT 5). Headline heap during steady-state M4A playback: free `59,800` / largest contiguous `28,660` — a ~4× improvement on the `9.8 KB / 7 KB` pre-rollback baseline that drove the whole eval. Whether the file is actually LC-AAC (and we mislabelled it in the eval) or HE-AAC decoded base-layer-only by libhelix-aac is open until we listen for SBR's high-frequency upscale on a known HE-AAC sample. Either way the rollback resolves the on-the-edge memory pressure that was the eval's central concern, regardless of HE-AAC's outcome.
- 2026-05-06: Extended testing surfaced a pre-existing config bug, *not* introduced by this rollback. The diagnostics row's core 0 graph reads ~0% always, and browser scrolling (pure main-loop work) moves the *core 1* sparkline. Confirmed: `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig` shows `CONFIG_ARDUINO_RUNNING_CORE=1` despite our `custom_sdkconfig = CONFIG_ARDUINO_RUNNING_CORE=0` override. The sister flag `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` *does* take effect (CPU sampling works), so the override mechanism is partly working — the running-core flag specifically isn't propagating. Loop task is running on core 1 alongside the audio task, contradicting `map.md:155-157`. Pre-existing since at least the v3 migration (the override was added then). Captured as a follow-up change `loop-task-core-pinning.md`; not blocking this rollback.

## Conclusion

Rolled back. Static-footprint reduction (~33 KB internal RAM, ~540 KB flash) and runtime headroom improvement (~4× contiguous heap during M4A playback) both substantially exceeded the Approach's predictions — the modular pipeline, three decoder backends, and Foxen FLAC together were carrying more weight than expected. Extended testing confirmed FLAC plays cleanly at the restored 3-slot ring depth without underruns; the dynamic `setRingDepth()` API the rollback dropped genuinely wasn't needed.

`map.md`'s Audio Formats node and Playback node already described the rollback target — the v3 migration's owed catch-up resolves automatically, no map edits required.

One pre-existing config bug surfaced during extended testing (loop task running on the wrong core) — captured as the follow-up change `loop-task-core-pinning.md`. The rollback didn't introduce it.

Final shipped version: `0.14.0`.

### Proposed `CHANGELOG.md` entry

```
## 0.14.0 — 2026-05-06

- FLAC playback no longer underruns, and multi-format track-change chains no longer crash — two regressions that arrived with the v3 framework migration are now fixed. Behind the scenes: the audio library swap from that migration is reversed (back to `ESP8266Audio` + the in-house M4A demuxer) while keeping the v3 platform and calibration-free CPU readings. ~33 KB of internal RAM and ~540 KB of flash are freed up at boot. HE-AAC M4A files (typical iTunes Store downloads) remain unsupported on this hardware and need re-encoding to LC-AAC or MP3 to play.
```
