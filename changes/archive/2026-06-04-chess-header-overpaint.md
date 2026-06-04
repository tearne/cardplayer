# Chess header overpaint

**Mode:** Wander

## Intent

The header strip occasionally renders on top of the chess board. Root cause: `pollBattery` redraws the header when the displayed battery level changes, and its overlay-guard only checks `g_show_help` rather than the unified `overlayActive()` (which now covers chess too). Tighten it to use `overlayActive()` so chess — and any future overlay — is respected for free.

A similar `!g_show_help` guard in the first-keypress path is technically vulnerable to the same class of bug, though impossible to trigger in practice (chess can only be entered after the first key has already been pressed). Tighten that too for consistency.

## Log

- The Intent's `g_show_help` is stale — the keymap overhaul retired that flag for the `Screen` model. The actual guard in `pollBattery` was `g_screen != Screen::KeyReference` (the descendant of the old check); replaced with `!overlayActive()`, which still suppresses the KeyReference redraw and now also covers Chess/Settings/Standby.
- The first-keypress path turned out to have *no* guard at all (not `!g_show_help`); added `!overlayActive()` for consistency as intended.
- Built clean at 0.26.28; awaiting hardware check (open chess with a track playing so the battery level can tick, confirm the header no longer paints over the board).

## Conclusion

Completed as intended — both header-redraw paths now gate on `overlayActive()`. The work diverged from the Intent's description in two ways, both recorded in the Log: the `g_show_help` flag it named was already retired (the guard had become `g_screen != Screen::KeyReference`), and the first-keypress path was fully unguarded rather than weakly guarded. The fix mapped cleanly onto the current `Screen` model regardless. No mapped concept is affected.

Shipped at **0.26.28**.
