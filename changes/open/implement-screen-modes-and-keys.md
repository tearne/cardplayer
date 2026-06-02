# Implement Screen-Mode Tree and Ctrl-only Keymap

## Intent

Implement the design settled in the review change `changes/archive/2026-06-02-screen-modes-and-keys.md`. That change produced the model; this one builds it.

In scope:

- Replace the scattered `g_show_*` flags and the long if/else dispatch chain with a single canonical screen state arranged as the agreed tree, with one uniform `` ` `` back-out rule (and Standby entered by backing out from the Main root).
- Apply the key-category model: a global transport & display set (pause, skip/seek on `[`/`]`, volume on `-`/`=`, brightness on `Ctrl+-`/`Ctrl+=`) that passes through every screen except text-entry and modal contexts; navigation, text entry, and mode-switch categories scoped as designed.
- Remap to Ctrl + unshifted base keys only: retire Fn entirely and the Shift-only command symbols (`?` `{` `}` `~` `+` `_`), keeping Shift purely for typing. `[`/`]` become tap = skip, hold = seek.
- Behaviour changes confirmed in the review: skip/seek pass through the menu/editor screens; Chess lets transport through without exiting.
- Fix the stale `Ctrl+A` comment at `src/main.cpp:4610`.
- Catch the map up once the new structure exists — flesh out the `Alarm` node and represent the screen-tree / keymap model (per-node negotiation).

The `Ctrl+letter` mnemonics (`Ctrl+E` settings, `Ctrl+R` reindex, etc.) were left provisional by the review and are finalised here.
