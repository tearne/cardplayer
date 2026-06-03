# Alarm wake stops at end

**Mode:** Wander

## Intent

Follow-up to `alarm-tweaks` (archived 2026-06-02), found in map review. The intended Enter-to-wake behaviour was "let the alarm track keep playing, and when it reaches the end stop." The first half works, but once Enter returns to Main the track is subject to the normal auto-play-next setting (default on), so at end-of-track the folder advances instead of stopping.

Make the woken alarm track stop when it ends, regardless of the auto-play-next setting — for both Enter-while-firing and Enter-from-snooze. Manually starting or skipping to another track afterwards cancels the one-shot stop, so only the woken alarm track is affected.
