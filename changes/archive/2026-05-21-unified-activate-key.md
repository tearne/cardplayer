# Unified browser activate key

**Mode:** Formal

## Intent

Simplify browser navigation: a single key (`/`, semantically "into") activates whatever's highlighted — descend a directory or start an audio track. `enter` is no longer used in the browser; pause/resume stays on `space`. Reduces the keymap's mental model and consolidates the cursor-on-playing-track edge case (already a no-op for re-activation) into one path.

## Approach

### `/` becomes the universal browser activate

`/` already descends directories. Extend it to call `startPlayback` for audio entries (i.e. wire it to `activateSelection`). Holding the key auto-repeats today; the existing "no-op when this track is already playing" guard inside `activateSelection` keeps repeated firings harmless. Enter wiring in the browser is dropped.

### `enter` keeps its non-browser roles

Settings activate, search-result play, reset-modal confirm, and any other overlays that bind enter keep their behaviour. Only the browser path stops using enter.

### `space` unchanged

Pause/resume as today, including pass-through in Settings and viz overlays.

## Plan

- [x] Wire `/` (in the plain-keys browser dispatch) to call `activateSelection` for audio entries (currently only descends directories).
- [x] Remove the browser-side `enter` → `activateSelection` wiring (line ~3347).
- [x] Remove now-unused `descend()` helper (subsumed by `activateSelection`).
- [x] Drop the no-op guard in `activateSelection` for already-playing tracks — `/` on the playing track now restarts from the beginning for consistency.
- [ ] Update the `Controls` node of `map.md` post-build (per per-node negotiation).
- [x] Smoke test: descend dirs with `/`, start an audio track with `/`, re-press `/` on the playing track (now restarts from the beginning), pause with `space`, resume with `space`, confirm `enter` is inert in browser, confirm `enter` still works in Settings / search / reset modal.

## Conclusion

Browser activation consolidated onto `/`. The browser-side `enter` binding is gone; `space` keeps its pause/resume role. `/` on an audio entry — including the currently-playing one — starts that track from the beginning, which is the consistent gesture across all entries. The previous no-op-on-already-playing guard (originally added when `enter` was the activate key) is removed.

**Documentation impact:** the `Controls` node of `map.md` describes the old binding ("`enter` — start playing… / no-op on the already-playing track" and "`,` / `/` — step out / enter highlighted directory"). Needs updating to reflect the new model. Pending per-node negotiation.
