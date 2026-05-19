# Playback controls cleanup

**Mode:** Formal

## Intent

Review and tighten the play / stop / pause / navigation behaviour. Three things flagged by the user:

1. **Stop on Enter doesn't earn its place.** Pressing Enter on the currently-playing track currently stops playback. Pause (space) already covers "temporarily halt"; navigating to another track auto-stops the current. The "stop" affordance has no obvious user-facing role. Decide whether to remove, replace (e.g. restart from start), or leave.

2. **Jumping while paused should update the progress bar.** The seek actions (`1`–`0` jump-to-tenth, `[` `]` held-seek) appear to work while paused — but the footer progress bar doesn't redraw, so the user can't see where they jumped to. Fix the visual feedback.

3. **Audit for other consistency issues** in the play / pause / seek / skip / advance interactions while we're touching this area.

## Approach

### Drop Stop as a binding; Enter is idempotent on the playing track

Enter on a directory descends. Enter on an audio file starts playing it (replacing whatever's currently playing). Enter on the *already-playing* track is a no-op — predictable, no surprise restart, no click-trauma.

`stopPlayback` stays as an internal function (used by track end, error paths, `startPlayback`'s teardown) but is no longer reachable from any user binding. Pause (space) still covers "halt for a moment". Navigating to a different track and pressing Enter implicitly stops the current one via `startPlayback`'s built-in teardown.

### Redraw the progress bar after every seek, paused or not

`jumpToTenth` (`1`–`0`) and `pollSeekKeys` (held `[` `]`) currently rely on `loop()`'s 500 ms progress-bar redraw — but that redraw is gated on `playing && !g_paused`. So while paused, the user seeks but the bar visually freezes at the old position. Fix: have seek actions call `drawSlotProgress()` + `presentRows(footerY(), footerH())` themselves, regardless of paused state.

### Track skip moves from `Fn+,` / `Fn+/` to `{` / `}`

Currently track-skip requires the Fn modifier, which sits inconveniently next to other commonly-used keys. The shifted `[` / `]` (i.e. `{` / `}`) become plain bindings for previous / next track. Adjacent to the held-seek bindings, so all four position-related actions (seek backward/forward + skip back/skip ahead) sit on the same key pair. The `Fn+,` / `Fn+/` bindings come out.

### Consistency sweep — other touch-points

- Track skip already calls `drawFooter()`, so the bar resets to 0 on skip. Unchanged.
- Toggle pause already calls `drawFooter()`, so the play/pause-coloured bar swaps correctly. Unchanged.
- Volume / font-size / wrap-toggle redraws the relevant slot only — unchanged.
- "stopped" label in the footer name region still appears at natural folder exhaustion, which is the only path that reaches it now. Behaviour preserved.

## Plan

### Tasks

- [x] In `activateSelection`, change the "already playing" branch from `stopPlayback()` to a no-op
- [x] In `jumpToTenth`, redraw the progress slot after seeking (paused or not)
- [x] In `pollSeekKeys`, redraw the progress slot after each seek tick (paused or not)
- [x] Bind `{` and `}` to `skipTrack(-1)` / `skipTrack(+1)`; remove the `Fn+,` / `Fn+/` bindings
- [x] Update help screen text — `Enter` no longer mentions stop; replace `Fn+,` / `Fn+/` lines with `{` / `}`
- [x] Update the Controls node in `map.md` — drop the Enter-stops-track wording; update the track-skip binding line

## Log

- 0.17.31: all six tasks landed. `pollSeekKeys` also got an opportunistic cost reduction — was calling full `drawFooter()` + `presentFrame()` (whole canvas push) on every 100 ms held-seek tick; now just `drawSlotProgress()` + `presentRows(footerY(), footerH())`. Same visual outcome at a fraction of the SPI cost.
- 0.17.32: skip-while-paused was deactivating the pause. Threaded a `start_paused` parameter through `startPlayback`, set inside the same mutex critical section as `g_gen` so the audio task never sees a transient `g_paused=false` between generator-swap and skipTrack's restore. `skipTrack` now captures `g_paused` before the call and passes it through.

## Conclusion

Playback controls tightened across four user-visible improvements:

- **Enter** on the already-playing track is a no-op (no surprise stop / restart). Stop is gone as a binding.
- **Track skip** moved from `Fn+,` / `Fn+/` to plain `{` / `}`, adjacent to held-seek.
- **Progress bar** updates after `1`–`0` jump and `[` / `]` held-seek regardless of paused state.
- **Paused state** is preserved across track skip — the next track starts paused at zero.

Documentation: Controls node in `map.md` updated as part of the change. Help screen updated.

**Changelog entry:**

> Playback control cleanup. `Enter` on the currently-playing track is now a no-op (was: stop). Track-skip moves from `Fn+,` / `Fn+/` to plain `{` / `}`, adjacent to the held-seek keys. Progress bar in the footer now updates during seek and jump-to-tenth even when paused. Skipping tracks while paused keeps the pause active — the next track starts at zero, ready to resume on space.
