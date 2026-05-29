# Chess cursor input

## Intent

Replace the current `e2e4`-style typed-coordinate input in [Chess](#chess) with a visual cursor on the board. A cyan selector outlines the cursor square; `;` / `.` / `,` / `/` move it (matching the browser / settings navigation pattern); `enter` picks the piece, a second `enter` completes the move; backspace cancels a pick-up. Re-using `enter` here is a deliberate exception to the player-wide "no enter" rule — chess has its own bindings and the audio player's policy stays intact.

The intent is to make the chess UI feel like the rest of the device — direct cursor movement rather than typing notation — and to free the user from having to know coordinates.

In scope:

- Cursor rendering on the board (cyan outline).
- Two-stage select: pick a piece, then place it.
- Cancel after pick-up.
- Replace the input-buffer indicator in the side panel with a hint reflecting the new flow ("pick / place" or similar).

Out of scope (this change):

- Promotion piece chooser — auto-promote-to-queen stays for now.
- Highlight legal target squares for the picked piece (worth doing, but a separate decision).

## Approach

### Key mapping

`;` up, `.` down, `,` left, `/` right — extends the browser's `;` / `.` up-down convention into 2D. `enter` picks then places. `del` (backspace) cancels a pick-up.

### Edges: clamp, not wrap

Matches the browser-cursor convention and avoids a jump that obscures whether the user has reached the edge.

### Pick / place / re-pick

`enter` on an empty square or an opponent piece while nothing is held is a no-op. `enter` on one of the user's own pieces picks it; on another own piece while a piece is held, switches the selection.

### Render

Cyan 2 px outline inside the cursor square. When a piece is held, a second inner outline (proposing yellow) marks the held state.

### Panel changes

Replace the "input / ..." line with a single state line: "pick" or "place" — visual cursor makes the coordinate redundant.

### Cleanup

Remove `g_input` and `parseAndPlay()` from `chess.cpp` — input is now cursor-driven.

## Plan

- [x] Add cursor state (file, rank, held-piece-square) to `chess.cpp`
- [x] Render cyan cursor outline; yellow inner outline when a piece is held
- [x] Replace key handler: `;` `.` `,` `/` move, `enter` pick / place / re-pick, `del` cancel
- [x] Replace side-panel "input" line with "pick" / "place" state indicator
- [x] Remove `parseAndPlay` and `g_input`

## Log

- Cursor resets to e1 on every chess::enter — fresh ergonomics, doesn't persist.
- Illegal target square = silent no-op, picked piece stays held so the user can try another square without re-picking.
- Held outline drawn before cursor so cyan stays on top when both land on the same square.
- Version bumped 0.22.0 → 0.22.1.

## Conclusion

Completed as planned, no deviations.

**Changelog entry:**

```
## 0.22.1 — 2026-05-25

- Chess: visual cursor replaces typed `e2e4` notation. `;` `.` `,` `/` move a cyan square selector around the board; `enter` picks a piece (yellow inner outline marks it held), a second `enter` places. `del` cancels a pick-up. Illegal target squares are a silent no-op — the held piece stays held so you can re-aim without re-picking.
```

**Documentation impact:** the `Chess` map node mentions `e2e4` notation — needs catch-up to describe cursor-driven input. Negotiate as a single-node map edit after archive.
