# Chess header overpaint

**Mode:** Wander

## Intent

The header strip occasionally renders on top of the chess board. Root cause: `pollBattery` redraws the header when the displayed battery level changes, and its overlay-guard only checks `g_show_help` rather than the unified `overlayActive()` (which now covers chess too). Tighten it to use `overlayActive()` so chess — and any future overlay — is respected for free.

A similar `!g_show_help` guard in the first-keypress path is technically vulnerable to the same class of bug, though impossible to trigger in practice (chess can only be entered after the first key has already been pressed). Tighten that too for consistency.
