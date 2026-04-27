# Persisted state

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
- font notch
- chrome-minimal toggle
- wrap-names toggle

Future UI knobs extend the record by adding keys; absent keys read as their compiled-in default, so older saves remain forward-compatible without explicit versioning.

### Save timing

Mutators (volume change, track start/stop, UI toggle, directory change) set a dirty flag. A periodic flush in the main loop writes to NVS once per second when the flag is set, so a burst of keypresses coalesces into a single write. The emergency-shutdown path performs no save; whatever was dirty at the moment of cutoff is lost — consistent with the Intent.

### Load and fallbacks

`loadState()` runs in `setup()` before the first volume apply, before `loadDir()`, and before any playback start. Each item falls back to its current compiled-in default when absent.

Folder fallback: open the saved path with the existing scanner; if it doesn't exist or scans empty, silently load `/`. Track fallback: if the saved file no longer exists, start with no playback.

### Reset

A reset key shows a confirmation modal that takes over the screen (similar to the existing help overlay). On confirm, the persisted record is cleared from NVS, in-memory state is reset to compiled-in defaults, playback stops, the browser returns to root, and the screen redraws. On cancel, the modal dismisses and nothing changes.

### Module placement

A small `persistence` section in `main.cpp` exposing four functions: `loadState()` (setup), `markStateDirty()` (called from mutators), `flushStateIfDirty()` (called from `loop()`), `resetState()` (called from the confirm path of the reset modal). Single Preferences namespace `"player"`.

### Map

A new map node describing persistence is a per-node negotiation after the build, not part of this change's Plan.

## Unresolved

1. **Track behaviour on boot** — should the persisted track auto-play from byte 0, or only be cued (folder loaded, cursor on it, but silent until the user presses Enter)? Auto-play risks surprising the user who powered on after charging; cue-only is quieter but loses the "resumes where it left off" feel.

2. **Browser cursor position** — persist which entry was highlighted in the saved folder, or just the folder? Not in the Intent list; raising because it's adjacent and cheap.

3. **Save debounce window** — 1 second proposed as the coalescing interval, also the worst-case loss window on a normal power-down. OK?

4. **Reset key binding** — which key? Existing single-character bindings already in use: `;`, `.`, `,`, `/`, `<`, `?`, `[`, `]`, `1`–`0`, `enter`, `space`, `-`, `=`, `+`, `_`, `` ` ``, `\`, `'`. A free key with low collision risk would be e.g. `~`, `*`, `#`, `&`, or a Fn-modified combination. Preference?

5. **Reset modal interaction** — confirm/cancel keys. Suggest `enter` to confirm, any other key (or `space`/`esc`) to cancel. The existing help overlay dismisses on any key, but a destructive action wants explicit confirmation. Suggested wording: "Reset all saved settings? Enter = yes, any other key = no".
