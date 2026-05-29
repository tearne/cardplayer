# Chess reset game

**Mode:** Formal

## Intent

Let the user abort the current chess game and start fresh — useful when a position is hopeless, a misclick spoils things, or the user just wants a new game without playing the current one out. Today the only path to a new game is reaching mate / stalemate and pressing `n` on the game-over screen.

Likely shape: a dedicated key while chess is active that opens a brief confirmation ("reset game? y/n" or similar — keeps an accidental press from nuking a game in progress), then on confirm calls the existing `newGame()` and stays in chess mode with a fresh board. Specific key and confirmation flow to be decided in Approach.

## Approach

### Trigger: `Ctrl+R`

Mnemonic, explicit modifier so hard to misfire, and parallels the existing `Ctrl+H` entry convention. Plain `r` would clash with the "any unconsumed letter exits chess" rule.

### Detection inside chess

`chess::handleKey` already receives the full key state from the host. Add a `state.ctrl` + `'r'`/`'R'` branch at the top of the handler, before the other key processing.

### Confirmation flow

Mirrors the existing player reset modal: `/` confirms, any other key cancels. Esc cancels (consistent with esc closing the chess overlay being a no-op while a confirm is open). Reset is destructive; a confirm is non-negotiable.

### Confirmation state

A single `g_confirm_reset` bool in chess.cpp. The handler enters confirm mode on Ctrl+R, leaves it on either branch. Cursor and held state are untouched by cancel; on confirm they reset via the existing `newGame()`.

### Visual

Centred modal box overlaid on the chess screen, drawn at the end of `render` so it sits on top of board and panel. Solid dark fill, framed in the same yellow as the held-piece outline (already a "warning" colour in chess). Text: `RESET GAME?` and a small hint `"/ confirm, else cancel"`.

### No special handling at game-over

The existing `n`-key new-game path on the game-over screen is left untouched; Ctrl+R also works there because `newGame()` is idempotent re: end state.

## Plan

- [x] Add a `g_confirm_reset` flag to `chess.cpp`
- [x] Detect `Ctrl+R` at the top of `chess::handleKey` and enter the confirm state
- [x] In the confirm state: `/` calls `newGame()` and exits the state; any other key exits the state with no change
- [x] Draw a centred yellow-framed "RESET GAME?" modal in `render` while the flag is set

## Log

- Confirm key changed from `/` to Enter (return) during testing — user request. Modal hint text now reads "return = confirm" at size-2 to match. Modal resized 180×56 → 210×64 to fit the wider hint. Did not use a Unicode return-arrow glyph: Font0 is ASCII-only and would render the codepoint as garbage; used the word "return" instead. 0.22.9 → 0.22.10 patch bump for the test hand-back.

## Conclusion

Shipped as 0.22.10. The Approach's `/`-confirms decision was overridden during testing in favour of Enter (return); the rest landed as planned.

### Proposed changelog entry

```
## 0.22.10 — 2026-05-28

- Chess: Ctrl+R while in chess opens a yellow-framed "RESET GAME?" confirmation. Return commits a fresh game; any other key (including Esc) dismisses the prompt without changing the board. Provides an explicit reset path that previously only existed implicitly via reaching mate / stalemate.
```
