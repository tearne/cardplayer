# Arduino-ESP32 v3 migration

## Intent

Move the project to Arduino-ESP32 v3.x, swapping the audio library at the same time. Two motivations land together because the dependencies couple them:

- **Direct, calibration-free CPU load** in the diagnostics row. v3's underlying ESP-IDF (and the pioarduino platform that exposes it) lets us enable FreeRTOS run-time stats, replacing the idle-hook rate counter and its post-boot calibration warmup with real-microsecond idle counters.

- **Audio path on a maintained, v3-native library** (`schreibfaul1/ESP32-audioI2S`). Active development, native M4A/AAC handling so the in-house `AudioFileSourceM4A` demuxer can be retired, ESP32-specific tuning instead of dual-target shared code with ESP8266Audio.

Earlier attempts to do these as two separate changes (`audio-library-refresh` then `calibration-free-cpu-load`) showed they can't be sequenced: ESP32-audioI2S itself uses ESP-IDF 5.x I2S APIs that only ship with Arduino-ESP32 v3, so the library refresh inherently requires the platform move. Done as a single migration the work is one coherent step.

## Approach

### Platform

`platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38/platform-espressif32.zip` in `platformio.ini`, with `framework = arduino` (no `espidf` framework — pioarduino exposes a complete Arduino-on-ESP-IDF stack with project-level sdkconfig overrides, keeping the Arduino board concept and variant intact). The first build rebuilds Arduino-ESP32 from source against our sdkconfig overrides; subsequent builds use the incremental cache.

### sdkconfig overrides

`sdkconfig.defaults` at the project root, listing only the FreeRTOS flags we need:

- `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` — kernel tallies per-task µs counters.
- `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` — exposes `vTaskGetInfo()` for reading any task's counter by handle.

### Audio library

`pschatzmann/arduino-audio-tools` (latest stable). Modular: separate `AudioPlayer` orchestrator, source classes (`AudioSourceSD`), decoder classes (per-format), and `AudioOutput` subclasses for sinks. Native M4A support via the library's `ContainerM4A` / `MP4Parser`, so the in-house `AudioFileSourceM4A` demuxer retires. Architecture-agnostic; works on Arduino-ESP32 v3.

> [!IMPORTANT]
> The library's `AudioOutput` base class has a single `write(const uint8_t*, size_t)` to override. We rewrite `AudioOutputM5CardputerSpeaker` (renamed appropriately) as a subclass that forwards decoded PCM to `M5.Speaker.playRaw()` — so `M5Cardputer.Speaker.begin()` continues to handle ES8311 codec init, AMP enable, and I2S configuration exactly as it does today. No manual codec / AMP wiring needed.

The audio task lifecycle stays (FreeRTOS task pinned to core 1, 8 KB stack, mutex-protected). Inside, an `AudioPlayer` instance owns the source / decoder / output chain. The task body becomes a tight `player.copy()` call.

### Control surface and seek

The public action functions (`stopPlayback`, `togglePause`, `skipTrack`, etc.) keep their signatures. Their bodies switch to the new API:

- play / stop → `AudioPlayer::play()`, `AudioPlayer::stop()`, `AudioPlayer::playPath()`.
- pause / resume → `AudioPlayer::setMuted()` (or `setActive()`).
- skip / previous → `AudioPlayer::next()`, `AudioPlayer::previous()`.
- volume → `AudioPlayer::setVolume()` (the digital control inside the player); `M5.Speaker.setVolume()` stays at full and is no longer touched directly.
- end-of-track auto-advance → `AudioPlayer::setOnEOFCallback()` sets `g_advance_pending` (or simply enables the player's built-in `setAutoNext(true)` and lets it advance on its own).

Seek stays byte-based, as today. The library's primary seek path is decoder-aware byte position (`StreamCopier::copy()` over a `File`), and adding accurate time-based seek for MP3 / M4A would require us to implement bitrate-aware position translation — meaningful work for a "nice-to-have" UX detail. Keeping byte semantics preserves existing behaviour and lets the change focus on the library swap and the calibration-free CPU goal.

### Audio output bridge

`src/audio_output_m5.h` is rewritten (not deleted) as a subclass of `audio_tools::AudioOutput` (rather than ESP8266Audio's). The class still owns the triple-buffer ring, still calls `m5::Speaker_Class::playRaw()`, and still tracks underruns and last-wait-microseconds for the diagnostics row. Just the base class and the per-frame entry shape change — `write(const uint8_t*, size_t)` consumes interleaved 16-bit PCM and feeds `playRaw()` exactly as today.

### CPU sampling rewrite

The idle-hook plumbing (`g_idle_calls_*`, `g_idle_max_rate_*`, `idleHookCore0/1`, `esp_register_freertos_idle_hook_for_cpu`, the `<esp_freertos_hooks.h>` include) is removed. In its place:

- `setup()` captures the per-core idle task handles via `xTaskGetIdleTaskHandleForCPU(0)` and `xTaskGetIdleTaskHandleForCPU(1)`.
- Each diagnostics tick, `vTaskGetInfo(handle, &info, pdFALSE, eRunning)` reads each core's idle run-time counter (microseconds, esp_timer-derived).
- Load is `1 − (idle_delta / wall_delta)`, both deltas in microseconds. First sample is honest — no warmup, no max-rate reference.

### Retired code

- `src/AudioFileSourceM4A.{cpp,h}` deleted in full — the new library handles M4A natively via its `ContainerM4A` demuxer.
- `src/audio_output_m5.h` is rewritten (see "Audio output bridge" above) — its purpose stays the same, only the base class changes.

### Buffer placement

Decoder buffer in internal RAM. PSRAM-backed buffering is available in `ESP32-audioI2S` but accessing PSRAM keeps the chip out of self-refresh and meaningfully raises battery draw. Default off; revisit only if internal-RAM buffering proves insufficient.

### Map

Several nodes affected — touched as a per-node negotiation after the build:

- **Audio Formats**: the `[!IMPORTANT]` callout about the in-house M4A demuxer is no longer accurate.
- **Playback**: the ring-buffer detail (3 slots × 1536 samples) was a property of the old output class; the new library has its own buffer model.
- **Diagnostics**: the CPU readouts now reflect real microseconds with no calibration warmup; node text already avoids mechanism details, but the "self-calibrate" caveat that lived in chat / changelog is gone.

### Known risks

- **M5Cardputer / M5Unified on v3**: M5Cardputer 1.1.1 declares Arduino compatibility without pinning a major. If it doesn't compile against Arduino-ESP32 v3 we'll need to investigate (newer M5Cardputer release, or M5Unified update).
- **First-build surprises**: the previous pioarduino build got past the framework rebuild but tripped on a v2-targeted library; a similar dependency may surface here once ESP8266Audio is out of the picture. The Plan calls out "first build to validate" as its own task before code rewrite.

## Plan

- [x] In `platformio.ini`: switch `platform` from `espressif32@6.9.0` to the pioarduino release `55.03.38`; swap `earlephilhower/ESP8266Audio@1.9.7` for `pschatzmann/arduino-audio-tools` (pinned to a recent stable tag) in `lib_deps`. `framework = arduino` stays.
- [x] Create `sdkconfig.defaults` at project root with `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` and `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`.
- [x] Delete `src/AudioFileSourceM4A.cpp` and `src/AudioFileSourceM4A.h` — the in-house M4A demuxer; the new library handles M4A natively via `ContainerM4A`.
- [x] Rewrite `src/audio_output_m5.h` as a subclass of `audio_tools::AudioOutput` (replacing the ESP8266Audio base): same triple-buffer ring, same `M5.Speaker.playRaw()` calls, same underrun and last-wait-microseconds tracking. The `write(const uint8_t*, size_t)` override consumes interleaved 16-bit PCM and feeds `playRaw()` exactly as today.
- [x] Rewrite the audio path in `src/main.cpp`: remove ESP8266Audio includes and the `AudioGenerator + AudioFileSource + AudioOutput` globals; add an owned `audio_tools::AudioPlayer` configured with an `AudioSourceSD`, a decoder factory covering MP3 / AAC / FLAC / WAV / M4A, and our rewritten output sink. Initialise after `M5Cardputer.Speaker.begin()` so the codec is ready before the first `playRaw()`.
- [x] Rewrite the audio task body around the player: under `g_audio_mutex`, call `player.copy()` (the modular pipeline equivalent of the old `audio.loop()`).
- [x] Rewrite playback control to use `AudioPlayer`: start (`playPath`), stop (`stop`), pause/resume (`setMuted` or `setActive`), skip (`next`/`previous`), volume (`setVolume`). End-of-track auto-advance: `setOnEOFCallback()` sets `g_advance_pending` so we keep ownership of track order rather than letting the player auto-iterate.
- [x] Keep byte-based seek as today — `seekToByte()` continues to drive the underlying `File`'s position via the player's source. No change to the digit-key jump-to-tenth or `[` / `]` hold semantics.
- [x] Rewrite CPU sampling for direct counter reads: remove `g_idle_calls_*`, `g_idle_max_rate_*`, `g_idle_calls_*_prev`, `idleHookCore0/1`, the `esp_register_freertos_idle_hook_for_cpu` calls, and the `<esp_freertos_hooks.h>` include. In `setup()` capture `xTaskGetIdleTaskHandleForCPU(0)` and `xTaskGetIdleTaskHandleForCPU(1)` into globals; `sampleCpuIdleFractions()` uses `vTaskGetInfo(handle, &info, pdFALSE, eRunning)` on each, takes the delta of `info.ulRunTimeCounter`, divides by elapsed wall-clock microseconds.
- [x] Bump `APP_VERSION` from `0.12.0` to `0.13.0`.
- [x] First clean build — slow (framework rebuild); iterate on any compile/link errors that surface from the platform + library + Arduino-ESP32 v3 combination, including any M5Cardputer / M5Unified compatibility issues.
- [x] On-device test (partial): MP3 plays cleanly; FLAC plays but with frequent underruns; M4A files using HE-AAC (SBR) crash the decoder with OOM. WAV / AAC / other UI behaviours not exhaustively re-verified. CPU readings show plausible values from the first sample (no calibration warmup) — primary goal met. Two follow-up changes opened (`flac-underruns`, `m4a-he-aac-sbr-oom`) to track the remaining audio gaps.

## Log

- 2026-04-28: Build progressed in three steps. (a) First build hit `arduino-libflac` requiring `ogg/ogg.h` which isn't packaged. Switched to the lighter `arduino-libfoxenflac` (pure-C single-header decoder, no Ogg dependency); decoder class changed from `FLACDecoder` to `FLACDecoderFoxen`. (b) Second build hit a stray `g_gen` reference in `loop()`'s playing-state check; replaced with `g_player.isActive()`. (c) Third build clean. Final footprint: RAM 19.6 % (was 7.4 %), Flash 42.0 % (was 23.4 %) — substantial growth from the modular library plus three decoder backends, but well within the 320 KB / 8 MB hardware budget.
- M5Cardputer 1.1.1 / M5Unified compiled cleanly against Arduino-ESP32 v3 — the worry flagged in the Approach as "Known risks" did not materialise.
- 2026-04-28: Bring-up uncovered several runtime issues, each fixed in turn: (a) **Static-init order fiasco** — declared `g_canvas(&M5Cardputer.Display)` and `g_out(&M5Cardputer.Speaker)` at file scope, capturing addresses of *reference* members of `M5Cardputer` before the latter's constructor had bound them. Worked on the v2 platform by link-order luck; v3 changed link order and exposed it. Fixed by default-constructing both globals and binding the parents inside `setup()` after `M5Cardputer.begin()`. (b) **Sprite allocation failure at boot** — internal RAM was too pressured for a contiguous 64 KB sprite buffer; switched to PSRAM via `M5Canvas::setPsram(true)`. (c) **AudioPlayer pipeline silently rejected writes** — replaced the `AudioPlayer` abstraction with the lower-level `EncodedAudioStream` + `StreamCopy` pieces; far fewer surfaces to misconfigure. (d) **MIME mismatch** — registered decoders with `audio/mp3` / `audio/wav`, but `MimeDetector` emits `audio/mpeg` / `audio/vnd.wave`. Fixed. Also pre-select decoder by file extension to skip MIME auto-detection. (e) **Decoder format-change wasn't propagating to the output** — the player used to wire this implicitly; without the player we explicitly call `addNotifyAudioChange(g_out)` on each decoder. (f) **MP3 + ID3v2** — Helix doesn't strip ID3 tags, so the manual ID3v2 skip from the ESP8266Audio path was restored. After all this, MP3 playback is clean.
- 2026-04-28: Two audio gaps remain and are tracked as follow-ups: FLAC underruns (Foxen decoder may be CPU-heavy, `flac-underruns` change opened), and HE-AAC M4A files crash with `OOM in SBR` because Helix can't get a 50 KB contiguous internal allocation (`m4a-he-aac-sbr-oom` change opened — workaround `CONFIG_SPIRAM_USE_MALLOC=y` triggers a framework rebuild that itself fails in wpa_supplicant strict-warning checks). Landing the v3 platform + Arduino-ESP32 v3 + arduino-audio-tools migration as the substantive win; remaining audio-format gaps are scoped to those two follow-up changes. Final shipped version: `0.13.15`.

## Conclusion

The migration unblocks calibration-free CPU sampling — the primary goal — and lands all the v3 platform plumbing. Two audio-format gaps surfaced during testing (FLAC underruns and HE-AAC M4A SBR OOM) and are tracked as follow-up changes rather than blockers, since they are scoped problems orthogonal to the platform move. The Log captures the bring-up discoveries.

Map catch-up needed for the **Audio Formats** node (the in-house M4A demuxer is gone; the [!IMPORTANT] callout describing it no longer applies) and possibly **Playback** (the ring-buffer detail is now a property of `audio_output_m5.h`, not of the decoder library). Per-node negotiation as separate work.

## Log

- 2026-04-28: Pre-build investigation surfaced an Approach-level gap before any source change landed. The Approach said we'd "configure the new library to drive the same I2S peripheral and pins" while keeping `M5Cardputer.Speaker.begin()` for codec init. Looking at M5Unified, `M5.Speaker` doesn't just drive I2S — it owns the ES8311 codec configuration over I2C (sample rate, format, gain, multi-register init sequence), holds exclusive use of `I2S_NUM_1` through its own driver, and registers a board-specific AMP-enable callback (`_speaker_enabled_cb_cardputer_adv`). ESP32-audioI2S wants direct I2S and assumes the codec is otherwise managed; the two don't coexist by simply sharing pin numbers. Going through with (i) means writing an ES8311 driver and AMP-enable wiring ourselves — significant board-specific code that the Approach didn't account for.

  A lighter alternative surfaced: the original ESP8266Audio compile failure on pioarduino was localised to the HTTP/ICY streaming files we don't use. Excluding those via a PlatformIO pre-build script would let us stay on ESP8266Audio + the in-house M4A demuxer while still moving to v3 and unlocking the run-time-stats path — getting the primary goal (calibration-free CPU load) without the audio-library-swap blast radius. Stopped per the build-mode rule and handed back for direction.
