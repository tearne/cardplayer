# Alarm tweaks

**Mode:** Wander

## Intent

Three small refinements to how a firing alarm looks and how it's dismissed:

- **White clock while sounding.** When the alarm is actually sounding, the standby clock turns white, giving an unmistakable visual cue that the alarm is live.

- **Enter = wake to the track.** Pressing Enter while the alarm is firing lets the current alarm track keep playing through to its end, then stops. No restore of the previous state — the user is up, and the music sees itself out.

- **`` ` `` (Esc) = silence and restore, paused.** Pressing `` ` `` stops the alarm track and returns to the saved previous track state. On returning, playback does not resume automatically — the device comes back paused where it left off.

## Log

- 0.24.6 (patch). All three tweaks done, both envs build clean. (1) Firing turns the big clock time **white** (`COL_CLOCK_FIRING`); standby/snooze keep the amber. (2) New `acceptAlarm()` for Enter-while-firing: leaves the alarm track playing (no snapshot restore), settles volume on the configured target unless the user nudged it, and lets the track stop naturally at end; beep-fallback fires just stop the beep. (3) `dismissAlarm()` (the Esc / `` ` `` path) now always returns **paused** regardless of the prior play state — dropped the now-unused `g_alarm_prior_paused` snapshot field. Firing's on-screen prompt rewritten to three lines ("Enter: keep playing" / "Esc: stop" / "any key: snooze") since Enter and Esc now differ.
- Decision not in the Intent: **Enter while *snoozing*** still dismisses-and-restores (now paused) — the "keep playing" behaviour only applies while the alarm is actually sounding, since the snoozed track is paused, not playing. Autostop (1 hr) also routes through the paused restore.
- 0.24.7 (patch). User asked for **Enter-from-snooze to start the alarm track from the beginning** instead of dismissing — superseding the snooze decision above. Added `wakeFromSnooze()` (restarts the slot's track from the top via `startPlayback`, settles wake volume, leaves it playing) and refactored the shared exit into `wakeToMain()` + `settleAlarmWakeVolume()`, reused by `acceptAlarm()`. Autostop still routes through the paused restore. Both envs build clean.
- 0.24.9 (patch). Firing-screen prompt wording: Enter line changed from "Enter: keep playing" to **"Enter: dismiss"** at the user's request (their label for leaving the alarm; behaviour unchanged — Enter still keeps the track playing). Briefly added then reverted a snooze-screen "Enter: dismiss" hint — user clarified they meant the firing screen.
- 0.24.10 (patch). Fixed the clock time **jumping 6px** between standby/firing/snooze: firing sat at y=22, the others at y=28. Unified to a single fixed `big_y = 22` so the time holds position across all three clock states; firing's three hint lines still clear it.
- 0.24.11 (patch). Top gap looked cramped at y=22. Lowered the unified time to `big_y = 28` (the balanced standby position) and packed firing's three hints tighter against the bottom edge (SCREEN_H -49/-33/-17) so they still clear the lower time. The firing-hint count caps how low the time can go.
- 0.24.12 (patch). Snooze countdown nudged down (`below_y + 14`) so it's centred in the space below the time rather than crowding it; standby next-alarm hint unchanged.

## Conclusion

Shipped at 0.24.12. Scope grew through testing beyond the three opening tweaks: Enter-from-snooze became "restart the alarm track from the top" (a deliberate reversal of the initial decision to leave snooze alone), the firing Enter label was set to "Enter: dismiss", and a clutch of clock-layout fixes followed once the white time and three-line firing prompt exposed an existing wrinkle — the time hopping 6px between standby/firing/snooze. Those settled on a single fixed time position (y=28) shared by all three clock states, with firing's hints packed against the bottom edge and the snooze countdown lowered.

The dismiss/wake split is the substantive design point: `dismissAlarm()` (Esc) now always returns paused, while two "wake to the music" paths — `acceptAlarm()` (firing, keeps the track playing) and `wakeFromSnooze()` (snooze, restarts from the top) — share a `wakeToMain()` + `settleAlarmWakeVolume()` tail rather than restoring the pre-alarm snapshot.

**Documentation impact:** the `Alarm` map node is still a TODO placeholder. This change adds real behaviour (white-clock cue, the dismiss-vs-wake key split, unified clock layout) that belongs there — folds into the screen-mode / alarm map catch-up already deferred from `implement-screen-modes-and-keys`.

Changelog entry added under 0.24.12.
