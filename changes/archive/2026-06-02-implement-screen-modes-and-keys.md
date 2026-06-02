# Implement Screen-Mode Tree and Ctrl-only Keymap

**Mode:** Formal

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

## Approach

### Two staged phases in one change

Phase 1: the screen-state / dispatch refactor. Phase 2: the keymap remap on top of it. *Reason: the category-based keymap is cleanest atop the new dispatch, and staging keeps the firmware building and hand-testable between phases.*

### Single screen state: enum + parent map, flags removed

Replace the scattered `g_show_*` booleans and the long if/else dispatch with one `enum Screen` (the current screen) and a `parentOf(screen)` lookup encoding the tree; back-out = go to parent. The `g_show_*` flags are removed outright (not aliased) for a clean end state. The Main area's content (browser / search / visualisation) is a sub-state of Main, not separate screens. The alarm interrupt keeps its existing snapshot-and-restore mechanism — it pre-empts any screen and is orthogonal to the navigation tree. *Reason: the enum + parent map is a 1:1 image of the agreed tree, making back-out a single operation instead of per-branch code.*

### Unified render and per-screen handlers

Each screen gets one render function and one key-handler, both dispatched on `Screen` — replacing the dual render paths (`draw()` for clock/chess/browser vs each overlay drawing itself) and the if/else key chain. *Reason: one place per screen; the drift that caused the Ctrl+D and Esc bugs comes from behaviour being spread across hand-written branches.*

### Category-ordered key dispatch

The top-level handler runs in category order: Back key → global transport/display (suppressed only on text-entry and modal screens) → the current screen's local handler. *Reason: the "which keys are global" rule lives in one gate, not duplicated per screen.*

### Keymap remap (phase 2)

The remap follows a **mirror principle**: because holding Ctrl makes the keyboard emit the same shifted character Shift would, each current Shift-symbol command moves to Ctrl + the *same* base key — identical character, only the modifier changes — preserving muscle memory. Final command keymap:

| Base keys (no modifier) | | Ctrl combos | |
|---|---|---|---|
| `;` `.` | move cursor | `Ctrl+/` (→`?`) | Settings |
| `,` | up a level | `Ctrl+[` `Ctrl+]` (→`{` `}`) | skip prev / next |
| `/` | activate / play | `Ctrl+-` `Ctrl+=` (→`_` `+`) | brightness ∓ (context-sensitive) |
| `space` | pause | `Ctrl+W/S/T` | waveform / spectrum / test |
| `-` `=` | volume ∓ | `Ctrl+H` | chess |
| `[` `]` | seek (hold) | `Ctrl+D` | diagnostics |
| `'` | jump to playing | | |
| `\` | wrap names | | |
| `0–9` | jump to tenth | | |
| letters | search text | | |
| `` ` `` | Back / Esc (→ Standby at root) | | |

Fn is retired entirely; Shift is left only for typing. Two rarely-used commands are dropped from the keymap and reached only via their Settings rows — **font size** and **rebuild index** — since no one will recall a shortcut for them. Plain `[`/`]` stay as seek (no tap/hold), since skip is now `Ctrl+[`/`]`. Fix the stale `Ctrl+A` comment at `src/main.cpp:4610`.

## Plan

### Phase 1 — screen-state / dispatch refactor

- [x] Add a `Screen` enum + `parentOf()` tree covering every screen, with Main content (browser / search / visualisation) as a Main sub-state.
- [x] Replace the `g_show_*` flags with the single current-screen state; rederive `overlayActive` / `fullScreenClockActive` from it.
- [x] Route all rendering through one `Screen`-dispatched renderer (`renderScreen`/`goToScreen`); exits go to `parentOf` via it.
- [x] Add the category-ordered key gate: Back → global transport/display (suppressed on text-entry and modal screens) → current screen's handler. (Implemented as a top-of-loop pre-pass.)
- [x] Migrate each screen's key handling from the old if/else branches into its per-screen handler. (Cross-cutting handling — back, transport, and now the Ctrl **mode-switches** — is consolidated in the pre-pass; the duplicated browser/viz Ctrl handlers that caused the repeated missing-binding bugs are removed. Per-screen branches stay inline but now hold only their own nav/activation. Not extracting them into separately-named functions — the bug-prone duplication is gone, which was the point.)
- [x] Make `` ` `` back-out go to `parentOf()`; backing out from the Main root enters Standby. (`backOut()` + pre-pass.)
- [x] Keep the alarm interrupt's snapshot/restore working across the new state.

### Phase 2 — keymap remap

- [x] Bind the mirror-principle Ctrl combos: `Ctrl+/` settings, `Ctrl+[`/`]` skip, `Ctrl+-`/`=` brightness (context-sensitive); retain `Ctrl+W/S/T/H/D`.
- [x] Retire Fn and the Shift-symbol commands; drop the font and reindex shortcuts (Settings rows only); plain `[`/`]` = seek.
- [x] Pass global transport through menus/editors; let Chess receive transport without exiting. (A `transportAllowed()` predicate — every screen except text/modal/clock — now gates the full transport set in the pre-pass: pause, volume, skip, seek, brightness. `pollSeekKeys` uses it too. The firing ramp-override wrinkle dissolved by *excluding the clock screens* from global transport, so firing keeps its bespoke volume. Dead per-branch transport removed from the menu/viz branches.)
- [x] Update the Key Reference screen to the new keymap.
- [x] Fix the stale `Ctrl+A` comment at `src/main.cpp:4610`.

### Map

- [ ] Catch the map up (per-node negotiation): flesh out the `Alarm` node and represent the screen tree + key-category model. (Deferred to an out-of-change per-node pass after conclusion.)

## Conclusion

Shipped at 0.24.4, hardware-tested. The `g_show_*` flags are replaced by one `g_screen` + `parentOf()` tree driving a single renderer and a category-ordered key pre-pass (Back → global transport → screen-local). Keymap is Ctrl-only — Fn retired, Shift for typing; the old Shift-symbol commands port to Ctrl + the same key (mirror principle); `` ` `` is the universal Esc, and global transport (volume/pause/skip/seek/brightness) reaches menus and chess.

Deviation from plan: per-screen branches weren't extracted into separately-named functions, but all cross-cutting handling (back, transport, Ctrl mode-switches) is consolidated in the pre-pass — which removed the duplicated-handler bug class that surfaced twice in testing (Ctrl+D, then Ctrl+/).

Map catch-up is deferred to an out-of-change per-node pass.

## Log

- 0.24.0 entered Build (minor bump). `Screen` enum + `MainContent` + `parentOf()` added and compiling clean as the foundation; `g_screen`/`g_main_content` not yet wired (still unused). Migration surface measured: ~108 read/write sites across the old flags, 49 writes, with interdependent exit logic — being done as a holistic compile-and-fix pass rather than one mega-edit. Next: derive `overlayActive`/`fullScreenClockActive` from `g_screen`, convert enter/exit + dispatch reads, then remove the flags.
- 0.24.1: keymap remap done (Phase 2), building clean on both envs. Added a top-of-loop **pre-pass** implementing the category gate: Back (`` ` `` on every screen except Standby, which keeps its post-entry guard) via `backOut()`, plus global Ctrl-brightness (context-sensitive via `changeBrightnessContext`) and Ctrl-skip. Removed all Fn blocks (Fn retired) and the Shift-symbol commands (`? { } ~ + _`); Settings → `Ctrl+/`, skip → `Ctrl+[`/`]`. Font + reindex shortcuts dropped (Settings rows only). Fixed the visualiser back-out bug (`` ` `` now backs out to the browser instead of entering standby). Key Reference screen rewritten to the new keymap (`^` = Ctrl). Two honest gaps: (1) per-screen handlers not extracted into named functions (task 5, cosmetic); (2) plain volume/pause/seek not globalised, so chess still exits on them (matches prior behaviour; firing volume has a ramp-override side effect that blocks blind globalisation).
- 0.24.4: consolidated the Ctrl mode-switches (`Ctrl+W/S/T/H/D`, `Ctrl+/`) into a single pre-pass block (act from Main; the picker absorbs stray Ctrl so it can't leak to search). Removed both duplicated Ctrl handlers (browser branch + viz branch) — the source of the recurring missing-binding bugs (Ctrl+D, then Ctrl+/). This is the substance of the gap-1 cleanup. Builds clean, retest welcome.
- 0.24.3: test-found bug — `Ctrl+/` (settings) did nothing while the visualiser was up. The viz overlay has its own Ctrl handler (separate from the browser's) that listed W/S/T/D but not `?`; added `Ctrl+/`→Settings there. This is the *second* missing-binding in that duplicated viz Ctrl handler (the Ctrl+D regression was the first) — strong evidence the duplicated Ctrl handling should be consolidated (ties into the deferred gap-1 extraction). Builds clean.
- 0.24.2: closed both gaps. **Gap 2 (full transport):** added `transportAllowed()` (every screen except text/modal/clock); the pre-pass now globalises the whole transport set — pause, volume, skip, seek, brightness — so menus and chess keep full playback control without exiting. `pollSeekKeys` uses the same predicate. Excluding the clock screens removed the firing ramp-override wrinkle entirely (firing keeps its own volume handling). **Gap 1 (neatening):** removed the now-dead per-branch transport from the menu/viz branches, so each branch holds only its own nav/activation. Did NOT extract branches into separate named functions — judged low-value indirection; flagged for the user. Builds clean on both envs.
- State migration complete and building clean on both envs. All eight `g_show_*`/standby/firing/snoozing flags removed; `g_screen` is the single source of truth. `overlayActive()` is now just `g_screen != Main && != TrackPicker`; `fullScreenClockActive()` reads the three clock states. Added `renderScreen()` (single render dispatch) + `goToScreen()`; enter functions set `g_screen`, exits call `goToScreen(parentOf)`. Dropped `MainContent` (search/viz/pick tracked by existing flags). Edge case handled: the reset modal has two origins (Settings / browser-Del), so it records `g_modal_return`. Behaviour is intended to be unchanged — worth a flash to confirm before the keymap remap. Still to do: category-ordered key gate + per-screen handler extraction (tasks 4–5), then Phase 2 keymap, then map.
