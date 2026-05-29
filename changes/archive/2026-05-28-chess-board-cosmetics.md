# Chess board cosmetics

**Mode:** Wander

## Intent

Visual tweaks to the chess board:

- Darken the light squares so the alternating-square contrast reads better and the pieces stand out more.
- Add a margin between the rank-number labels (1–8 down the right edge of the board) and whatever sits to their right (today: the side panel). Currently the digits abut the panel and look cramped.
- Replace the letter glyphs for pieces (`P N B R Q K` / `p n b r q k`) with actual chess piece shapes. Unicode chess symbols were considered but ruled out — a scan of every M5GFX-bundled font (standard, GFXFF, Custom, lvgl, efont CJK, IPA) turned up no evidence of coverage for U+2654–265F. Going with **bitmap sprites**: one 12×12 (or similar) mono shape per piece kind (six total — pawn/knight/bishop/rook/queen/king), drawn in white or black at render time so the same sprite serves both sides.

## Log

- Light squares: `COL_LIGHT` 0xCE99 → 0xB596 (~70 % intensity of original).
- Rank-label margin: `PANEL_X` offset 6 → 11, gives ~4 px gap between the rank digits (which end at x=139) and the panel's left edge (x=144). Panel narrows from 102 to 96 px wide — `thinking`, `WHITE WINS`, and the result text all still fit.
- Piece sprites: 6 × 12 × 12 mono sprites (PAWN..KING in enum order), encoded as `uint16_t` rows. 144 bytes flash. `drawPieceSprite` paints set bits in white or black; replaces the `pieceGlyph` text path. Sprites are centred in the 16 × 16 square with 2 px margin all round.
- Built clean; not yet flashed.
- Version bumped 0.22.4 → 0.22.5.
- Iteration after first flash: light squares darkened further (0xB596 → 0xA514), dark squares lightened (0x6B0C → 0x7BCE) so the two converge but stay clearly distinct. Selector colour now state-dependent — blue (0x041F) in pick mode, cyan (0x07FF) in place mode, so the colour itself signals what the next `enter` will do. Held-piece yellow outline rewritten as a 2 px outline matching the selector's dimensions instead of the previous 1 px inset. Version bumped 0.22.5 → 0.22.6 (still part of this iteration).
- File / rank labels removed at user request. Panel offset reduced (`+11` → `+2`), panel widens from 96 → 106 px. Cleaner board, more room for status text. (Same 0.22.6 build.)

## Conclusion

Cosmetic pass on the chess board, iterated via flash-and-react rather than nailed down up front:

- Square palette retuned twice — first darken of `COL_LIGHT`, then both colours moved toward each other so the board reads as one piece while the contrast still carries.
- Selector colour split by state: blue while picking, cyan while placing. The colour change is now the load-bearing cue for "what does `enter` do next?". The held-square outline was redrawn to match selector dimensions so the two marks read consistently.
- Pieces are 12 × 12 mono bitmap sprites, six shapes, recoloured per side. Unicode chess symbols were probed first and ruled out — none of the bundled M5GFX fonts cover U+2654–265F.
- File / rank labels removed mid-iteration to free panel width — gained 10 px, board looks tidier.

**Changelog entry:**

```
## 0.22.6 — 2026-05-28

- Chess board cosmetics. Pieces are bitmap sprites instead of letters. Square palette retuned for better piece contrast (lighter dark / darker light squares). Cursor outline turns blue while picking and cyan while placing, signalling what the next enter press will do. Held-piece outline rewritten in yellow at matching dimensions. File / rank labels removed; panel widens from 96 to 106 px.
```

**Documentation impact:** the [Chess](#chess) map node still owes a catch-up from the earlier cursor-input change (typed-coordinate text is stale, and now glyphs / colours / labels have moved too). Worth a single per-node catch-up that picks up everything at once.
