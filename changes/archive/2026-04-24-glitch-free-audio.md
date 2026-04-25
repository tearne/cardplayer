# Glitch-free audio during navigation

## Intent

Stop audio from cutting out when the main loop does SD work (e.g. scanning a directory on navigation). Playback should remain smooth regardless of what the user is doing in the UI.

## Approach

### Decouple audio from the main loop

Audio decoding moves to a dedicated FreeRTOS task pinned to the other core. Specifics (core pinning, stack, priority, mutex) live in the proposed **Playback** node.

### No application-level SD mutex

Start without one. The Arduino SD library already locks per transaction; an app-level mutex would block audio completely during scans — opposite of what we want. If glitches persist after the task split, address via deeper buffering, yielding inside scans, or directory caching — not a coarser lock.

### Permanent diagnostics row in the header

Readouts (stack peak, ring fill, underrun count) are carried by the proposed **Diagnostics** node. They tune this change and stay useful during later development; they're worth keeping visible rather than debug-flag-gated.

### Map edits

**New top-level node — Playback (child of Application):**

```markdown
# Playback

[Up](#application)

Decoding of the current track runs on its own FreeRTOS task, pinned to the ESP32-S3's core 0, so that UI work on the main core (core 1, Arduino's default) cannot starve it. Decoded samples flow through a small ring buffer into the I2S driver.

**Detail**

- Task pinned to core 0, 8KB stack, priority 3 (above main loop at 1, below driver tasks).

- A mutex guards start/stop transitions of the generator and source; pause is a `volatile bool` without locking.

- Output ring buffer in `audio_output_m5.h` — three buffers of 1536 samples. Depth governs how long the task can be delayed before the user hears a gap.
```

**Updated node — Application (adds Down link to Playback and updates tree overview):**

```markdown
# Application

[Down](#screen-layout)
[Down](#playback)
[Down](#controls)

A minimal media player for MP3 and FLAC files on the M5Stack Cardputer.

​```
Application
├ Screen Layout
│ ├ Header
│ │ ├ Version
│ │ ├ Battery
│ │ └ Diagnostics
│ ├ Browser
│ └ Footer
├ Playback
└ Controls
​```
```

**Updated node — Header (grows to ~18px, new Down link, adjusted prose):**

```markdown
# Header

[Up](#screen-layout)
[Down](#version)
[Down](#battery)
[Down](#diagnostics)

A two-row strip at the top of the display (~18px total). The first row carries status indicators and transient notification banners. The second row carries development-time diagnostics.
```

**New node — Diagnostics (child of Header):**

```markdown
# Diagnostics

[Up](#header)

Second row of the header, showing live readouts useful during development. Present regardless of what the user is doing.

**Detail**

- **stk** — peak stack usage of the audio task (numeric, e.g. `stk: 3.1k`).

- **buf** — small horizontal bar showing current audio output ring-buffer fill. Near-full means decoding is ahead; emptying signals a starve.

- **underruns** — cumulative count of ring-empty events. Zero means no audible glitches.
```

**Browser** and **Footer** nodes unchanged textually — Browser loses one row of vertical space but its prose doesn't specify row counts.

## Plan

- [x] Add a FreeRTOS task for audio: pinned to core 0, 8KB stack, priority 3. On boot the task exists but idles until playback starts.

- [x] Add a mutex guarding `g_gen` / `g_src` transitions. `startPlayback` / `stopPlayback` take it while swapping; audio task takes it around each `g_gen->loop()` call.

- [x] Remove `g_gen->loop()` from the main loop — the audio task owns it.

- [x] Expose ring buffer fill and an underrun counter from `AudioOutputM5CardputerSpeaker`. Increment the counter when a sample would have been produced but the buffer already flushed/drained.

- [x] Grow `HEADER_H` from 10px to ~18px and shift the Browser area down accordingly.

- [x] Render the diagnostics row: `stk: N.Nk` (from `uxTaskGetStackHighWaterMark`), a small `buf:` fill bar, and an underrun counter. Refresh once per second.

- [x] Add the **Playback** node to `map.md` as a child of Application.

- [x] Update the **Application** node in `map.md` — add `[Down](#playback)` and update the tree overview to include Playback and Diagnostics.

- [x] Update the **Header** node in `map.md` — add `[Down](#diagnostics)`, update prose for the new two-row height.

- [x] Add the **Diagnostics** node to `map.md` as a child of Header.

## Conclusion

Two watchdog regressions surfaced on device and were fixed in build:

- The audio task's `taskYIELD()` between `loop()` calls couldn't schedule IDLE0, which has lower priority. Replaced with `vTaskDelay(1)` so the idle task always gets a slot.

- MP3s with large ID3v2 tags kept the decoder inside a single `loop()` call for seconds while scanning for the first audio frame, again starving IDLE0. Fixed structurally by swapping core assignments: audio task on core 1, main loop on core 0 via `-DARDUINO_RUNNING_CORE=0` (with `build_unflags` to remove the framework's default). The Playback node in `map.md` now names the watchdog-avoidance principle as the design rule, with core placement as an instance of applying it.

Other deviations from Plan:

- The Playback node was slimmed during the rewrite — mutex/pause specifics and ring-buffer internals turned out to be implementation rather than design, and dropped out.

- Stack peak displayed as bytes (`stk:3100`) rather than the proposed `stk:3.1k`; underrun label is `u:` to save horizontal space.

- The ring-fill bar shows last-flush wait time (clamped to 2ms) as a proxy for decoder headroom — longer waits mean the decoder is ahead of the speaker.

- Natural end-of-track handling in the audio task clears `g_play_path` but leaves the `g_play_dir` / `g_play_entries` / `g_play_idx` snapshot so `<` / `?` skip navigation still works.

Related: `slow-mp3-open.md` Intent refreshed to describe the ID3v2 root cause. The crash is fixed here; the UI freeze during decoder scan remains scope for that change.
