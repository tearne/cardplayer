# Alarm Clock

**Mode:** Formal

## Intent

Make the player usable as a bedside alarm clock.

A new Settings sub-menu houses everything alarm-related:

- The current time and day-of-week, displayed and editable (correcting drift between firmware builds, or first-time setup beyond the build-timestamp seed).
- Up to five alarms. Each alarm has:
  - A time (24-hour clock).
  - A selectable set of days-of-week it fires on.
  - A track that plays when it fires.
  - Its own volume.
- A configurable backlight brightness used while in alarm standby (separate from the normal Screen Idle brightness ramp).

A new dedicated key puts the player into **alarm standby**: a full-screen clock at the standby brightness, waiting for the next armed alarm.

When an alarm fires, the chosen track plays at the alarm's volume and loops, and the screen returns to its normal (non-standby) brightness. Any key dismisses; if nothing dismisses it, it stops automatically after one hour.

Bundled side change: render the current volume as an integer overlay on the volume bar, so the user can build muscle memory for their preferred level. Volume limits are unchanged.

## Approach

### Mode dispatch and entry key

Alarm standby is a new top-level mode flag (`g_standby_active`) alongside the existing overlay flags. Entered with **Ctrl+A**; exited by any key (which is also how the alarm itself is dismissed). *Reason: one obvious action for "make this stop / set me down for the night", not a chord.*

### Alarms fire from any mode

`pollAlarms()` runs from the main loop unconditionally — armed alarms fire regardless of what the user is doing (browsing, in chess, in settings, mid-track, in standby, or screen-off). On fire, the device pre-empts the current state: the prior screen is saved, playback (if any) is paused, and the alarm screen takes over. Dismiss restores the prior screen and resumes prior playback. *Reason: the bedside use case requires alarms to be reliable irrespective of how the device was left.*

### Settings sub-menu

A new `SR_ALARMS` action row in the existing Settings list opens a dedicated **Alarms screen** (full-screen, same yellow framing as Settings/Chess). The Alarms screen lists five rows (one per alarm slot) plus a "Set current time" row at the top and a "Standby brightness" row at the bottom. Selecting an alarm row opens an **Alarm editor** screen for that slot.

The editor lays out the four fields vertically — time, days, track, volume — using the same `;` / `.` cursor and `,` / `/` adjust idiom users already know from Settings. *Reason: three screen-deep nesting (Settings → Alarms → editor) is unusual but matches the natural information hierarchy and keeps each screen scannable. Reuses existing key bindings so nothing new to learn at the leaf.*

### Alarm storage

Each alarm is stored in NVS under the `"player"` namespace with packed keys per slot N (0..4):

- `aN_hm` — uint16, `(hour << 8) | minute`
- `aN_d`  — uint8 bitmask, bit i = day i fires (Mon=0..Sun=6)
- `aN_v`  — uint8 volume (0..g_volume_max)
- `aN_r`  — uint8 ramp seconds (0..60, 0 = no ramp)
- `aN_e`  — uint8 enabled flag (0/1)
- `aN_t`  — string, absolute track path

Standby brightness gets `stbybr` (index into existing `BRIGHTNESS_LEVELS`). *Reason: packing time as one key halves the NVS entries; brightness reuses the existing 8-step ladder so the Settings widget is identical to the normal brightness row.*

### Alarm row label

In the Alarms screen, each slot row is labelled by its config rather than slot number: `HH:MM MTWTFSS` where each day letter is shown if armed and underscore (`_`) if not. Disabled alarms show the same string dimmed. *Reason: the label is the config — no separate name needed, no guessing which slot is which.*

### Day-of-week

Day-of-week comes from a Zeller's-congruence calculation on the date read from the RTC. We do **not** trust the HYM8563's WeekDay register because (a) `seedRtcIfUnset()` writes a placeholder 0 and (b) any user time-correction would have to compute the correct day anyway. Calculating it on the fly is one helper function and is the source of truth everywhere — alarm firing logic, the standby-clock day display, the "set current time" UI's day label. *Reason: one path is always cheaper to keep correct than two.* The day-of-week bitmask is **Mon-first** (bit 0 = Mon … bit 6 = Sun); the editor lists Mon first.

### Alarm evaluation

A `pollAlarms()` runs once per second from the main loop (alongside `pollRtcClock`). It reads the current minute and weekday, and for each armed alarm checks whether the slot matches and we have not already fired in this minute (single `last_fire_minute` watermark per slot prevents repeated fires inside the same minute). Fire = enter standby-dismiss + start playback + ramp brightness back to user level. *Reason: per-second polling is cheap and avoids any RTC-alarm-pin wiring; one-minute granularity is the natural alarm resolution.*

### Fire behaviour

On fire: snapshot prior state (current mode flags + currently-playing track path + playhead + `g_volume`), set `g_alarm_firing`, ramp brightness to the user's normal level, take over the screen with the full-screen 24-hr clock (same renderer as standby), start the alarm track (see Volume ramp below), stamp fire-start time. *Reason: the time of day is the most useful piece of information at wake-up.*

While firing, the key map is:

- **Any key other than `` ` `` / Enter / `+` / `-`** → **snooze 8 min** (set `snooze_until = now + 8min`, stop playback, screen darkens to standby brightness, full-screen clock continues; when `now ≥ snooze_until` the alarm re-fires).
- **`` ` `` or Enter** → **dismiss** (stops playback, restores `g_volume`, but **keeps the alarm track loaded as the current track in normal mode** so the user can press play to continue listening if they wish; restores prior screen and brightness).
- **`+` / `-`** → adjust live volume mid-fire (does not dismiss or snooze).
- **Auto-stop** after `now - fire_start > 1 h` of cumulative firing time (snooze time excluded): treat as dismiss.

End-of-track during fire → restart same track (loop). *Reason: looping by restart works for every format we support.*

If the configured track cannot be opened (SD missing, file gone), the fire substitutes a **built-in beep tone** synthesised through the existing speaker stack (short on/off pattern repeated). The visible state still says the alarm is firing; dismissal/snooze behaviour is identical. *Reason: silent failure on a wake alarm would be worse than wrong sound.*

### Volume ramp

If the alarm's `ramp_s` is 0, playback starts at the configured volume. If non-zero, playback starts at `0` and the alarm raises `g_volume` linearly to the configured value over `ramp_s` seconds. Snooze re-fires reset the ramp. Mid-fire user volume tweaks (`+` / `-`) override the ramp from that point on.

### Snooze visibility

A snooze badge is added to the full-screen clock layout (small `Snooze M:SS` under the date — minutes:seconds counting down from 8:00 until the alarm re-fires), so the user can see how long until the alarm re-fires. The clock stays at standby brightness during snooze. *Reason: ambiguity ("did I snooze or dismiss?") is the common bedside frustration; one line removes it.*

### Set current time UX

The "Set current time" screen exposes all six fields (HH, MM, YYYY, MM, DD; SS resets to 0 on commit). Same `;` / `.` / `,` / `/` idiom; a final action row commits with `g_rtc.setDate()` + `g_rtc.setTime()` after computing the correct WeekDay via the Zeller's helper.

### Track picker

The track field opens the existing **Browser** in "pick" mode: same navigation, same filtering, `/` selects the highlighted track (no separate confirmation prompt — same key the user already presses to start playback in normal mode). *Reason: zero new key bindings, no surprise compared to normal browse.* The picker uses a distinct colour theme (different highlight / accent palette) so the user sees at a glance they're choosing a track for an alarm, not browsing for playback. *Reason: action affordance differs from normal browse (no playback starts on select), the visual cue prevents mistaken `/` presses.*

### Standby-screen rendering

Full-screen wall clock: big `HH:MM` centred, smaller weekday/date below, and a **next-alarm hint** ("next: Mon 07:00") on the bottom row computed from the armed alarms + current weekday. Re-drawn once per second from `pollRtcClock()` (extended to render the standby screen when active). On entry, ramp brightness to `g_standby_brightness_idx`; on exit, ramp back to `g_brightness_idx`. The existing idle-fade FSM is **suppressed** while in standby — the standby brightness is already a deliberate dimming choice. *Reason: idle fade fighting standby brightness would be confusing; the standby mode is itself the "I'm idle" signal.*

### Editor preview

The alarm editor's bottom row is a **Preview** action. Activating it dims the screen to standby brightness, waits 5 s (full-screen clock visible — same wake-up framing the user will see for real), then fires the alarm exactly as a scheduled fire would (including the ramp and snooze/dismiss key map). *Reason: gives an honest end-to-end test of the wake experience, not just an audio check.*

### Volume integer overlay

Render the integer `g_volume` as small text immediately to the left of the volume bar in the footer, redrawn from `drawSlotVolume()`. Two chars wide (max value 64). The bar geometry doesn't change; the integer goes in the existing padding. *Reason: in-place is the least intrusive — no layout reshuffle, no footer-width contention with the track-name marquee.*

## Log

- 0.23.0 entered Build.
- Foundation in: Zeller's `dayOfWeekMonFirst()`; `seedRtcIfUnset` writes the correct WeekDay; `Alarm` struct + `g_alarms[5]` with NVS load/save (`aN_hm`/`aN_d`/`aN_v`/`aN_r`/`aN_e`/`aN_t`); `g_standby_brightness_idx` + NVS `stbybr`; volume integer overlay added to the left of the bar.
- Halting here for an interim verification flash before tackling the UI surfaces. Foundation work is invisible (only the volume number is visible). Next blocks are big: 5 new screens, mode dispatch, fire state machine.
- 0.23.1: bug found in test — the track-playback path was never wired into the fire state machine. `fireAlarm` only set up the beep (which gated on an empty track), so a slot with a real track was silent (preview included). Wired track start into `fireAlarm` with beep fallback on open-failure, looping on end-of-track during fire, and stop/pause of the track in `snoozeAlarm`/`dismissAlarm`. Builds clean.
- Gap surfaced but NOT yet built: `fireAlarm` never snapshots prior playback (track/playhead/volume), so dismiss can't resume music that was playing before the alarm, nor restore the pre-alarm volume. Awaiting user direction on whether to add this.
- 0.23.2: test-feedback refinements. (1) Alarm-editor Track row now left-aligns the name after "Track:" and clips with an ellipsis instead of overpainting the label (`drawAlarmTrackValue`). (2) Track row: `,` clears the track back to beep — chose this over the proposed "system" fake-folder; simplest path, reuses the existing `,`/`/` idiom. (3) Track-pick browser now a coherent yellow theme (selection + both frame separators + scrollbar) via `COL_PICK_FRAME`/`browseFrameColor()`, matching Settings (browse=blue, search=green, pick=yellow). (4) Esc/`` ` `` now backs out of a sub-context (pick, then search) one level at a time, both plain and Fn+`` ` ``; standby clock only entered at the top of the browse hierarchy. Builds clean.
- 0.23.3: full-screen clock (standby / firing / snooze) was being overpainted by the waveform/spectrum push. `pollVisualisation()` now returns early when `overlayActive()`; viz state is preserved and resumes on clock exit. Builds clean.
- 0.23.12: fixed a regression — Ctrl+D (diagnostics) did nothing while a visualisation was active. The viz-overlay key branch's Ctrl handler covered W/S/T but had lost the D case (an orphaned "toggles in place" comment was left behind near the `` ` `` case, hinting it was dropped when the clock-entry key was added there). Restored Ctrl+D in the viz Ctrl branch (toggles in place, viz recomposes at the resized slot); removed the stale comment.
- 0.23.11: implemented the parked decision — dismiss now restores the pre-alarm state (option C). A fresh fire snapshots the user's volume always, plus the playing track's path/byte-position/paused-state when one was loaded; snooze re-fires keep that snapshot (`fireAlarm(slot, fresh=false)`). Dismiss reloads the prior track paused-and-seeked (mirroring the boot-resume at the saved playhead), resumes playing if it was, or stops cleanly if nothing was playing — and always restores the pre-alarm volume so the alarm's volume doesn't stick. Supersedes the 0.23.1/0.23.5 "keep the alarm track paused" behaviour. Builds clean.
- 0.23.10: simplified the firing hints to two mirrored "label: action" lines — "Esc/Enter: dismiss" over "other key: snooze". Dropped the backtick (reads as Esc to the user); tightened "Esc / Enter" to "Esc/Enter" so it fits one line (the spaced form was exactly 240px and clipped).
- 0.23.9: nudged the standby/snooze clock down (big_y 22→28) for better vertical balance against the corner status text and the two-line hint. The bottom is tight — the two size-3 hint lines already sit near the screen edge — so the hint line spacing dropped 26→24 to make room, and firing keeps big_y=22 (its dismiss prompt needs the space below, which the move would otherwise overflow). Per-mode big_y/below_y.
- 0.23.8: corrected a misread of 0.23.7 — the user meant the *next alarm's* weekday, not today's. Reverted the time-line layout (today's weekday back top-left, time back to size 7) and reformatted the next-alarm hint to two lines: "next:" on its own line, the alarm's weekday+time ("Sun 08:30") below. `nextAlarmString` now returns just the detail; the caller adds the label.
- 0.23.7: moved the weekday onto the big-time line (centred weekday+time group) per request. The size-7 time nearly filled the 240px width with no room beside it, so the time dropped to size 6 to make space; still the dominant element. Battery stays top-right, same colour as the weekday. The time now sits slightly right of screen-centre (the group is centred, not the time alone) — flag for the user to judge on device.
- 0.23.6: clock-screen readability tweaks. Repalette to brighter warm amber/orange (all blue=0) so it's legible at low standby brightness without raising the backlight — the dim deep-red was driving the user to crank it. Weekday moved to the top-left, matched in colour and size to the top-right battery (shared `COL_CLOCK_SECONDARY`). That frees the whole area below the time, so the next-alarm hint / snooze countdown now render at size 3 via a new `drawClockSubtext` that wraps to two lines on the most central space. Firing prompt kept at size 2 (screen is at full brightness during fire and it has more text) to avoid colliding with the snooze hint.
- 0.23.5: snooze now *pauses* the track instead of stopping it, so a dismiss from snooze leaves it paused-and-resumable — consistent with dismiss-while-firing. Clock still goes silent during snooze; re-fire restarts the track as before; beep unaffected (no loaded track to keep). Builds clean.
- 0.23.4: alarm-editor restructure (user-approved design tweaks). Collapsed the 7 day rows into one **Days** row (summary `MTWTFSS`, shared `formatDaysMask` helper) opening a new **Days sub-screen** (4th nesting level, accepted). Moved Track to the end of the field list and made it a multi-line block — `Track:  >` plus the full name wrapped/dimmed/indented (3-line cap); this required the editor's first variable-height row, so the draw loop now uses running-y plus `ensureAeCursorVisible`/`g_alarm_editor_top`. Picker now opens on the slot's current track (`navigateToPickTarget`) and always restores the main browse position on exit (pick commit switched to `restore_path=true`). Builds clean.

## To test (resume here)

Latest flashed/buildable version: **0.23.9**. Outstanding manual verification before this change can conclude:

**Editor restructure (0.23.4)**
- [ ] Editor shows a single "Days" row summarising the set as `MTWTFSS` (off days as `_`), matching the Alarms-list format; the 7 individual day rows are gone.
- [ ] `/`/Enter on the Days row opens the Days sub-screen (7 toggles + Back); `;`/`.` move, `,`/`/`/Enter toggle, Del/`/?/Fn+` back to the editor.
- [ ] Track row is now last (above Preview/Back); a set track shows `Track:  >` with the full name wrapped, dimmed and indented, beneath it (capped at 3 lines, `...` if longer).
- [ ] `,` on the Track row still clears to `(beep)` (one-line, no wrapped block).
- [ ] Editor scrolls correctly with the variable-height Track row (selected row always visible; Preview/Back reachable).
- [ ] Picker opens on the slot's current track (folder + highlight); empty slot or missing folder falls back to last position.
- [ ] After picking a track, the main music-browse position is restored (Settings detour doesn't move it); cancel restores it too.

**Track playback (0.23.1)**
- [ ] Preview a slot with a real track → the track plays (was silent).
- [ ] Track loops at end-of-track during a fire (doesn't advance to next file or go silent).
- [ ] Slot with no track, or a track whose file is missing/on a pulled SD → beep fallback sounds.
- [ ] Snooze pauses the track (clock silent), re-fire restarts it. Beep excluded throughout.

**Pre-alarm restore (0.23.11)**
- [ ] Nothing playing before the alarm → dismiss leaves a clean slate (no track loaded) and restores the pre-alarm volume.
- [ ] Music playing before the alarm → dismiss reloads that track at its old position, resuming if it was playing / paused if it was paused, at the pre-alarm volume.
- [ ] Same restore works dismissing from snooze, and on the 1-hour auto-stop.
- [ ] Prior track was on a now-missing/removed SD path → dismiss falls back to a clean stop (no crash).
- [ ] Hint text reads `Esc/Enter: dismiss` over `other key: snooze`.
- [ ] Volume ramp climbs 0→configured over `ramp_s`; `+`/`-` mid-fire overrides the ramp.

**Editor / pick UI (0.23.2)**
- [ ] Long track name shows `Track: <start>…` and never overpaints the "Track" label.
- [ ] `,` on the Track row clears it back to `(beep)`; `/` opens the picker.
- [ ] Track picker shows the yellow theme: selection highlight, top & bottom separators, and scrollbar all yellow (not blue/green).
- [ ] In the picker, Esc/`` ` `` (plain and Fn+`` ` ``) exits back to the editor — does NOT start the clock.
- [ ] In search, Esc/`` ` `` exits search rather than starting the clock.
- [ ] At the top of the browser (no pick/search), `` ` `` still enters the standby clock.

**Visualisation vs clock (0.23.3)**
- [ ] With waveform and/or spectrum enabled, enter standby (Ctrl+A) → clock is clean, no viz bleed at the sides.
- [ ] Fire an alarm (or preview) with viz enabled → clock stays clean while firing and while snoozing.
- [ ] Exit standby / dismiss → viz resumes normally.
- [ ] With a visualisation active, Ctrl+D toggles diagnostics in place (viz stays up, recomposes at the new size).

**Clock screen (0.23.6)**
- [ ] Standby clock is clearly legible at the standby brightness without raising the backlight (brighter warm palette).
- [ ] Weekday is top-left, battery top-right, same colour and size.
- [ ] Next-alarm hint below the time reads `next:` on one line and the alarm's weekday+time (e.g. `Sun 08:30`) on the line below, at size 3; snooze countdown likewise larger.
- [ ] Today's weekday is back top-left and the time is back at full size (size 7) — the 0.23.7 day-on-time-line experiment was reverted.
- [ ] Firing prompt and "other key: snooze" hint don't overlap.

**Open decision — RESOLVED (built in 0.23.11)**
- Decided: option C — dismiss restores the pre-alarm playback (track/position/paused) and volume. See the Pre-alarm restore test items above.

## Asides

- **Snooze/dismiss key hints under the clock when the alarm is firing.** Replace the "next: …" line with instructions ("`/Enter dismiss · any key snooze" or similar) so the user can tell what to do without remembering.
- **Shake-to-snooze** (future change). IMU-based gesture — detect a sharp wrist movement during fire to act as a snooze, complementing the keyboard map.

## Plan

- [x] Day-of-week helper from `(year, month, day)` (Zeller's), Mon=0..Sun=6.
- [x] `seedRtcIfUnset` writes correct WeekDay using the helper.
- [x] `Alarm` struct + `g_alarms[5]` + NVS load/save (`aN_hm`, `aN_d`, `aN_v`, `aN_r`, `aN_e`, `aN_t`).
- [x] `g_standby_brightness_idx` + NVS `stbybr`.
- [x] Add `SR_ALARMS` action row to Settings; opens Alarms screen.
- [x] Alarms screen: top row "Set current time"; five slot rows labelled `HH:MM MTWTFSS` (disabled slots dimmed); bottom row "Standby brightness".
- [x] Set-current-time editor: HH MM YYYY MM DD, commit writes RTC with computed WeekDay; SS reset to 0.
- [x] Alarm editor screen: time, days bitmask (Mon-first), track (opens picker), volume, ramp seconds, enabled toggle, preview action, back action.
- [x] Track picker: browser entered in pick mode with distinct colour palette; `/` returns the highlighted path to the editor.
- [x] `Ctrl+A` enters standby mode; any key exits (subject to fire/snooze rules when an alarm is sounding).
- [x] Full-screen clock renderer: big `HH:MM`, weekday + date, next-alarm hint, optional snooze countdown.
- [x] Standby mode applies `g_standby_brightness_idx`; idle-fade FSM suppressed while standby active.
- [x] `pollAlarms()` in main loop: per-second, per-armed-slot, `last_fire_minute` watermark, fires regardless of current mode.
- [x] Fire entry: snapshot prior state (mode + playing track + playhead + volume), set `g_alarm_firing`, ramp brightness to user level, show full-screen clock, start playback at alarm volume (or beep on missing track).
- [x] Volume ramp: linear `0 → configured` over `ramp_s`; user `+`/`-` overrides.
- [x] Fire key map: `` ` `` / Enter dismiss; `+` / `-` volume; any other key snoozes 8 min.
- [x] Snooze: stop playback, dim to standby brightness, badge `Snooze M:SS` on clock, re-fire when countdown hits zero.
- [x] Dismiss: stop alarm playback, restore prior `g_volume`, restore prior screen + brightness, leave alarm track loaded as current track for continued play.
- [x] 1-hour cumulative-fire auto-stop (snooze time excluded), treated as dismiss.
- [x] Missing-track beep: synthesised tone pattern via existing speaker stack, alarm UI continues normally.
- [x] Preview action: dim to standby, 5 s clock, then fire as a real fire.
- [x] Volume integer overlay (two chars) to the left of the volume bar in `drawSlotVolume()`.

Post-test additions (0.23.1–0.23.4):

- [x] Wire track playback into the fire path: start the slot's track, loop on end-of-track, beep fallback on open-failure; snooze stops it, dismiss pauses-but-keeps-loaded.
- [x] Track row left-aligns/clips the name (later superseded by the multi-line block); `,` clears the row to beep.
- [x] Track-pick browser uses a coherent yellow theme (selection, separators, scrollbar).
- [x] Esc/`` ` `` backs out of pick/search one level; standby clock only at the top of the browse hierarchy.
- [x] Suppress waveform/spectrum render while the full-screen clock is up.
- [x] Collapse the 7 day rows into one Days row + Days sub-screen; summary via shared `formatDaysMask`.
- [x] Track row moved to end of fields; multi-line wrapped/dimmed/indented name (3-line cap); variable-height editor scroll.
- [x] Picker opens on the slot's current track; main browse position always restored on pick or cancel.

## Conclusion

Shipped at 0.23.12, compile-clean but not yet hardware-verified — the To-test checklist is preserved here for a later pass. Dismiss restores the pre-alarm state (track, position, volume); the plan's "snapshot prior state" task was settled as option C.

Asides: firing key-hints implemented; shake-to-snooze spun off as a separate proposal.

Map: a minimal `Alarm (TODO)` placeholder added; full mapping deferred to the screen-mode review.



