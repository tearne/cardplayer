# Persisted state

**Mode:** Formal

## Intent

The device should remember user-facing state across power cycles so each session resumes where the last one left off. Persist:

- Volume
- Current track (the track itself, not the playback position within it)
- UI configuration (theme/colours, footer mode, font size, and similar knobs as the UI grows)
- Browser folder (fall back to the root if the folder no longer exists, e.g. after the SD card contents change)

The user can also reset the saved state on demand — a single keypress, gated by a confirmation modal, returns the device to its compiled-in defaults.

The emergency-poweroff path is absolute: nothing in the persistence mechanism may override or delay it, and an emergency shutdown does not save state. Persistence therefore happens during normal operation, not as a shutdown step.

## Approach

### Storage mechanism

Arduino-ESP32's `Preferences` library (NVS-backed key/value in flash). Independent of the SD card, robust against power loss between writes, ships with the core — no new dependencies.

### What gets persisted

A flat record, one key per item:

- volume
- playing track (full path)
- browser folder (full path)
- browser cursor index within the saved folder
- font size
- diagnostics-row visibility
- wrap-names toggle

Future UI knobs extend the record by adding keys; absent keys read as their compiled-in default, so older saves remain forward-compatible without explicit versioning.

### Save timing

Mutators (volume change, track start/stop, UI toggle, directory change, cursor move) set a dirty flag. A periodic flush in the main loop writes to NVS at most once every five seconds when the flag is set, so a burst of keypresses (or fast scrolling) coalesces into a single write. Acceptable worst-case loss window is the same five seconds. The emergency-shutdown path performs no save; whatever was dirty at the moment of cutoff is lost — consistent with the Intent.

### Load and fallbacks

`loadState()` runs in `setup()` before the first volume apply, before `loadDir()`, and before any playback start. Each item falls back to its current compiled-in default when absent.

Folder fallback: open the saved path with the existing scanner; if it doesn't exist or scans empty, silently load `/`. The saved cursor index restores when the folder loads cleanly; out-of-range falls back to 0.

Track behaviour on boot: if a saved track's path still exists, load it into the audio path but start paused. The footer shows the track name with the paused indicator; pressing space resumes. If the saved path no longer exists, start with no playback.

### Reset

`Del` shows a confirmation modal that takes over the screen (similar to the existing help overlay). On confirm, the persisted record is cleared from NVS, in-memory state is reset to compiled-in defaults, playback stops, the browser returns to root, and the screen redraws. On cancel, the modal dismisses and nothing changes.

(`Esc` would have been the natural choice but the Cardputer's keymap has no Esc entry; `del` is the physical key with the right semantic.)

### Module placement

A small `persistence` section in `main.cpp` exposing four functions: `loadState()` (setup), `markStateDirty()` (called from mutators), `flushStateIfDirty()` (called from `loop()`), `resetState()` (called from the confirm path of the reset modal). Single Preferences namespace `"player"`.

### Map

A new map node describing persistence is a per-node negotiation after the build, not part of this change's Plan.

### Reset modal interaction

`Enter` confirms the reset; any other key (including `Esc` again) cancels and dismisses the modal. Modal text: *"Reset all saved settings? Press Enter to confirm. Any other key cancels."*

## Plan

- [x] Create the persistence module: Preferences namespace `"player"`, four functions (`loadState` / `markStateDirty` / `flushStateIfDirty` / `resetState`), flat record covering volume, playing-track path, browser folder, cursor index, font size, diagnostics-row visibility, wrap-names
- [x] Wire `markStateDirty()` into every mutator: volume, track start/stop, directory change, cursor move, font-size toggle, diagnostics-row toggle, wrap toggle
- [x] Wire `flushStateIfDirty()` into the main loop with the 5-second coalescing window
- [x] Wire `loadState()` into `setup()` before the first volume apply, `loadDir()`, and any playback start; apply the documented fallbacks (folder → root, cursor → 0 on out-of-range, track → no playback when path missing)
- [x] Boot-resume: when a saved track path exists, load it via the normal start-playback path but begin in the paused state so the audio task doesn't decode until the user resumes
- [x] Add the reset confirmation modal: full-screen overlay with the prompt text; `Enter` confirms (clears NVS, resets in-memory state to compiled defaults, stops playback, returns the browser to root, redraws); any other key cancels
- [x] Bind `Del` to open the reset modal (the Cardputer keymap has no `Esc`; `Del` is the physical key with the matching semantic — captured in the Approach)
- [x] Bump `APP_VERSION` from `0.15.0` to `0.16.0`
- [x] On-device verification: fresh boot loads compiled defaults; after some changes wait >5 s and power-cycle, all items restore; saved track loads paused, space resumes it; missing saved folder falls back to root; out-of-range cursor falls back to 0; `Del` opens the modal, `Enter` resets, any other key cancels; emergency-shutdown cuts off without saving

## Log

- 2026-05-08: Approach proposed `Esc` as the reset trigger but the Cardputer's keymap has no `Esc` entry (`Keyboard_def.h` has `0x00` at the ESC position; the `KeysState` struct has no `esc` flag). Switched to `Del` — physical key, unused, has the right "wipe" semantic. Updated Approach inline; Plan task wording updated to match.
- 2026-05-08: Compile error during wiring — `stopPlayback` (around line 380) calls `markStateDirty` defined later in the file. Added a forward declaration near the top alongside the existing `enterBatteryLowState` forward decl. Minor housekeeping; nothing structural.
- 2026-05-08: User-test refinements applied without intermediate patch bumps (process slip — refinements should each bump patch so the user sees the version reflecting the latest build): modal font enlarged to size-2 (matches emergency-shutdown style); diagnostics row's compile-time and post-reset default flipped from visible to hidden. Final shipped version stays at `0.16.0`.

## Conclusion

Shipped. Persistence covers the Approach's items plus the cursor catch-up. Reset bound to `Del` since the Cardputer keymap has no `Esc`. Two test-time refinements (modal font, diagnostics default) folded in. Map node for persistence intentionally out of this change's scope — per-node negotiation will follow.

Final shipped version: `0.16.0`.

### Proposed `CHANGELOG.md` entry

```
## 0.16.0 — 2026-05-09

- Settings now persist across power cycles — volume, current folder + cursor, font size, wrap-names, diagnostics-row visibility, and the playing track. The device boots back into the saved track loaded paused (press space to resume); a missing folder or track falls back gracefully. Press `Del` to bring up a confirmation prompt that wipes the saved settings and returns the device to defaults. The diagnostics row now defaults to hidden; toggle with `` ` ``.
```
