# Screen blank when idle

**Mode:** Formal

## Intent

Save power by turning the display off after a minute of inactivity, and wake it as soon as the accelerometer detects motion (a deliberate pick-up of the device). Keypresses also count as activity and reset the idle timer. Audio playback continues throughout — only the screen sleeps.

Backlight is the dominant power draw on the display side (~20–30 mA on this 1.14" TFT), so blanking it during idle should buy a meaningful slice of battery life back during longer listening sessions. The Cardputer ADV has a built-in IMU which makes motion-wake a natural complement to the keypress wake.

## Approach

### Three-stage idle timeline

The screen has three states based on time-since-last-activity:

- **0 – 60 s** — full brightness (default).
- **60 – 120 s** — dimmed to 50 % brightness (≈ value 128 of 255). Visual cue that the device is idling but still glanceable.
- **120 s +** — backlight off (brightness 0) and the display controller put to sleep. The canvas keeps existing in RAM but redraw / push calls are skipped.

The transitions happen in a single periodic check inside `loop()` that compares `millis() - g_last_activity_ms` against the two thresholds.

### Activity sources

Two things reset `g_last_activity_ms` to the current `millis()`:

1. **Keypress.** Any captured key in the existing keyboard-handler path. Easiest to hook at the very top, before the mode-specific dispatch.

2. **Motion.** A new periodic poll reads the IMU at ~10 Hz, computes the magnitude delta between consecutive samples, and treats anything above a threshold (default ~0.05 g, tunable) as motion.

Any non-zero activity transitions the screen back to full brightness and wakes the display controller if it was asleep.

### Skip redraws while off

When the screen is in the off state, the various poll functions that push frames (`pollMarquee`, `pollWaveform`, `pollDiagnostics`, the progress-bar timer) skip their work. The full screen redraw on wake re-establishes the visible state.

Dim state still redraws normally — content stays current so the user reading at a glance sees up-to-date marquee / progress.

### Don't pause audio

Audio playback is unaffected by screen state. The whole point is to listen with the screen off.

## Plan

### Tasks

- [x] Track `g_last_activity_ms`; update from the keyboard handler and from a new IMU motion poll
- [x] Track `g_screen_state` (Full / Dim / Off) and update in a periodic check inside `loop()`
- [x] IMU poll at ~10 Hz: read accel, compute delta-magnitude vs previous sample, set activity if > threshold
- [x] Wake transition: restore brightness to 255 and `Display.wakeup()` if it was asleep; force a redraw
- [x] Skip frame pushes (`pollMarquee`, `pollWaveform`, `pollDiagnostics`, progress-bar timer) when state is Off
- [x] Set initial brightness to 255 at boot for an explicit default that matches the "Full" state

## Log

- 0.17.42: built the state machine with instant brightness transitions and `Display.sleep()` at the off threshold.
- 0.17.43: dim level tweaked from 50 % to 25 % per user.
- 0.17.44: replaced instant brightness writes with a linear-ramp system — 10 s fade for dim and off transitions, 1 s ramp for wake. Dropped the `Display.sleep()` / `Display.wakeup()` calls; brightness 0 covers the user-visible "off" state without the controller-sleep complexity.

## Conclusion

Screen has three idle states gated on time-since-last-activity (60 s → dim, 120 s → off). Transitions ramp brightness linearly — slow on the way out (10 s), quick on wake (1 s). Activity is either a keypress or an IMU motion sample above a small threshold (~0.05 g), polled at ~10 Hz. Audio playback is unaffected throughout.

Screen-off skips frame pushes from the marquee, waveform, diagnostics-row, and progress-bar redraws — saves the SPI / CPU work while the panel is dark.

**Changelog entry:**

> Screen idle-blanking. The display dims to 25 % after a minute of no keypress or motion, fades fully off after two minutes. Pressing a key or picking up the device fades it back to full in ~1 s. Audio keeps playing throughout. Battery life noticeably extended during long listening sessions.
