# Per-core load: honesty and conscious split

**Mode:** Explore

## Intent

The diagnostics row's per-core CPU graph reports one core idle at ~0% all the time, which means the dashboard isn't telling the truth about how the device uses its two cores. Worse, we don't actually know with confidence which application concerns — UI rendering, audio decode, keyboard polling — run where, because what we see on the screen contradicts the historic intent recorded in the map.

The end goal is two things at once: per-core CPU readouts that mean what they say (so the user can use them to reason about load and headroom), and a conscious decision about which parts of the application run on which core. We're not bound by the historic split — "UI on core 0, audio on core 1" is one option, not a given. If a different split is better for this hardware and this workload, we should land there with eyes open.

The work in this change is investigation, not the fix. Figure out where each piece runs today, understand what the framework actually gives us, weigh the options, and decide what we want. Implementing the chosen split is a follow-up change; this one ends with a recorded decision and a clear next step.

## Approach

### Scope

Every app concern — main loop body, audio decode, keyboard polling, M5 background tasks (battery I2C, touch) — plus the task watchdog. The current core-0-idle watchdog target was an original reason for today's split, so any new split needs a matching watchdog story.

### Method: empirical first

A `xPortGetCoreID()` print at each suspect site settles where things run today. Reading framework `sdkconfig` and init explains why. Empirical first avoids the trap we just fell into — assuming configured intent is operating reality.

### Framework constraint

Where M5's own tasks run is a constraint, not just data. Moving framework-internal work pulls further from framework assumptions; the decision defers to that pinning unless there's a strong reason to override.

### Output

Conclusion records the chosen split and matching watchdog setup. Trivial implementation folds in; otherwise open a follow-up change with the decision as its Intent.

## Plan

**Topics**

- Where each app-owned site runs today (loop, audioTask, keyboard, M5 update path).
- Why the existing override didn't take effect on `CONFIG_ARDUINO_RUNNING_CORE` while a sister flag did.
- M5's own tasks: which ones, where pinned.
- Watchdog target implied by each candidate split.

**Done when**: a Recommendation is written into this change with the chosen split, the matching watchdog target, and a clear next step — either folded-in implementation tasks added to this Plan and built here, or a follow-up change opened with the recommendation as its Intent.

## Findings

### Where each app-owned site runs today

Empirical, via `xPortGetCoreID()` probes at `audioTask` entry and `loop()` first iteration:

```
[core-probe] audioTask running on core 1
[core-probe] loop() running on core 1
```

Both app-owned tasks run on **core 1**. Core 0 has no app-level work; only `IDLE0` runs there. This explains the diagnostics row's permanent `cpu 0: 0%` reading and why browser scrolling moves the core 1 sparkline.

### Why the override didn't take effect

Bigger than the original framing. **The whole `custom_sdkconfig` mechanism is a no-op on this build.** Both flags are sitting at their pre-built defaults:

- `framework-arduinoespressif32-libs/esp32s3/qio_qspi/include/sdkconfig.h` (the file compilers actually consume): `#define CONFIG_ARDUINO_RUNNING_CORE 1`, `#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1`.
- `sdkconfig` and `sdkconfig.orig` are effectively identical (only differ in `# default:` comment lines).
- `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` was already the pre-built default, so CPU sampling worked by coincidence — not because our override propagated.

Our `pre:scripts/patch_log_wrapper.py` runs every build (visible in build logs), but the framework rebuild that's *meant* to apply `custom_sdkconfig` either isn't being triggered or isn't updating the include directory the build uses. The mechanism the v3 migration relied on appears to have silently regressed.

### M5 framework task pinning

Only one M5-internal task is in play on this hardware:

- **`spk_task`** (M5Unified `Speaker_Class`): `task_pinned_core` config defaults to `~0` — *unpinned*. FreeRTOS schedules it on whichever core is available; in practice tends to follow the caller (`audioTask` on core 1 → `spk_task` likely on core 1 too).

Inactive on this hardware: `mic_task` (no mic in use), `Panel_EPD` task (color LCD, not EPD), `Panel_CVBS` task (no TV-out).

So M5's only constraint on the split is the speaker task, which has no opinion. The framework imposes no real pressure on which core hosts what.

### Watchdog target per candidate split

The task watchdog watches one core's idle task and panics if a task on that core doesn't yield within the timeout. Three candidate splits:

- **A — Status quo (everything on core 1)**: loop, audioTask, spk_task all on core 1; core 0 unused. Watchdog on core-0-idle is meaningless (nothing on core 0 to starve). Watchdog on core-1-idle would catch real starvation but is risky — audio must yield in time. Honest version: disable the watchdog or accept it watches nothing.
- **B — Restore original split**: loop pinned to core 0, audioTask on core 1, spk_task floats. Watchdog on core-0-idle becomes meaningful again — loop body must yield. Matches `map.md` as written.
- **C — Audio on core 0, UI on core 1**: framework default leaves loop on core 1; pin audioTask to core 0 instead. Watchdog moves to core-1-idle (UI must yield). Audio gets the dedicated core, UI gets the watchdog protection.

Implementation cost differs:

- (A) is no code change — accept reality, update the map, retire or repurpose the watchdog config.
- (B) needs a *working* override mechanism for `ARDUINO_RUNNING_CORE` (currently broken). Options: fix `custom_sdkconfig`, use `-DARDUINO_RUNNING_CORE=0` build flag (the `platformio.ini` comment notes this was tried before and hit a redefine error — needs revisiting now that `custom_sdkconfig` does nothing), or a different approach.
- (C) is one-line — change `AUDIO_TASK_CORE` in `src/main.cpp` from `1` to `0`. Watchdog target adjustment in `sdkconfig` if applicable.

## Recommendation

**Option C: audio on core 0, UI on core 1.**

Reasons:
1. Cheapest implementation by a wide margin — one constant change in our own code, no fight with the broken sdkconfig mechanism.
2. Audio is the time-critical work; giving it a dedicated core (with no UI competition) is the strongest version of the original "no-blocking" intent.
3. Honest about the framework's actual behaviour: Arduino-ESP32 puts `loop()` on core 1 by default, the M5 speaker task floats; we work *with* that rather than against it.
4. Watchdog moves to core-1-idle, which now has real work (UI loop, M5 update, browser draws) so starvation is a meaningful signal again.

The broken `custom_sdkconfig` mechanism is a separate concern — worth a small follow-up change to either fix or remove from `platformio.ini`, but not a blocker for the split decision.

## Next step

Implementation is one-line plus a watchdog adjustment — fold into this change rather than open a follow-up. Adding to the Plan:

- [x] Change `AUDIO_TASK_CORE` from `1` to `0` in `src/main.cpp`
- [x] Update `map.md` Playback node text
- [x] Investigate watchdog config: confirmed framework defaults — `CONFIG_ESP_TASK_WDT_INIT=1`, 5 s timeout, watches `IDLE0` only (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=1`, no CPU1 equivalent set). With audio now on core 0, this catches a hung decoder. UI on core 1 has no watchdog but is naturally polling. Retargeting watchdog would need the (broken) `custom_sdkconfig` mechanism — left at framework default.
- [x] Remove the `[core-probe]` prints (after verification confirmed swap; refined to `0.14.2`)
- [x] Bump `APP_VERSION` from `0.14.0` to `0.14.1` (patch — small implementation change, no user-visible feature shift)
- [x] On-device verification: `[core-probe]` lines (before removal) showed `loop() running on core 1` and `audioTask running on core 0` — pinning took effect cleanly. Subsequent probe-removal build (`0.14.2`) staged for ship.

Separately tracked (not part of this change):
- The broken `custom_sdkconfig` mechanism — open a follow-up change once we've decided what to do about it.
- Tap cleanup belongs to `changes/open/internal-ram-optimisation.md` (the `[mem] steady`, `[mem] post_start/stop`, `[mem.fast]` taps are all from that change's instrumentation).

## Conclusion

Investigation done-when met: a Recommendation was recorded (option C — audio on core 0, UI on core 1), and the implementation was small enough to fold in. The chosen split lands cleanly with one constant change in `src/main.cpp` — no fight with the broken `custom_sdkconfig` mechanism, no follow-up change needed for the implementation itself.

Surprises beyond the Findings:

- The `custom_sdkconfig` override mechanism is fully a no-op on this build; `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` was working by coincidence (matches the framework default). Captured as a separately-tracked follow-up — the `platformio.ini` override block needs either a working mechanism or an honest "left at framework defaults" admission.
- The watchdog's framework default — `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=1` only, no CPU1 equivalent — happens to align well with audio on core 0: catches a hung decoder. UI on core 1 has no watchdog but is naturally polling.

Map updated in-flight (Playback node line 157) — tightly bound to the implementation since the prior text was directly contradicted by the new split.

Final shipped version: `0.14.2`.

### Proposed `CHANGELOG.md` entry

```
## 0.14.2 — 2026-05-07

- Audio decode now runs on core 0 with the UI on core 1; the per-core CPU readouts in the diagnostics row are honest about the split. Previously both ran on core 1 (the cyan core 0 line stayed flat at 0%). The task watchdog now meaningfully protects the audio decoder.
```
