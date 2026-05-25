# Chess game

**Mode:** Wander

## Intent

Add a chess game playable alongside music — a side activity for when the user wants something to do while a track is on. Game state persists across reboots (likely saved to SD) and survives the user switching away to the browser or other screens. CPU opponent provided by **micro-Max** as the first experiment — chosen for its tiny RAM/flash footprint given current ~18% free-heap headroom.

First-test scope: get micro-Max integrated, basic board rendering, move input via the keyboard, and persistence. Strength tuning, polish, and UX refinement come later.

**Keyboard boundary.** Chess must not interfere with the device's core music/browser controls. Entry via a dedicated chord (proposing `ctrl-h`); `esc` exits; any key not explicitly consumed by chess mode also exits (so the user can hit a transport key and have it just work). State is preserved on exit so the game is resumable.

## Log

- micro-Max licence check came back empty — author's page and source header carry no licence statement, so default copyright applies. Pivoted to a homegrown engine. Original Intent text still names micro-Max; reconcile at Conclusion.
- PoC scope reduced to "CPU plays a random legal move". Full legal move generation is implemented (castling, en passant, promotion-to-queen, check/checkmate, stalemate). Strength is the next decision once integration is proven.
- Wired into `main.cpp`: `Ctrl+H` enters, any unconsumed key exits with state preserved. Chess takes top priority in the key-dispatch chain when active. Viz overlays are snapshot-dismissed on entry; user restores them with Tab as usual.
- Persistence rides Preferences/NVS under the `chess` namespace — board, side, castling rights, ep square, last-move string. Not SD as originally guessed; NVS is simpler and the state is tiny.
- Naming collision found: Arduino headers define `sq(x)` as a macro for squaring. Renamed local helper `sq(file, rank) -> idx(file, rank)`.
- Build: RAM 22.8 %, Flash 27.4 % (+~5 KB flash). Compiles clean; not yet flashed.
- Version bumped 0.21.68 → 0.22.0.

## Conclusion

PoC kept. Chess sits in its own module (`src/chess.{h,cpp}`) with the host wiring confined to a handful of edits in `main.cpp`: a draw delegate, an overlay-flag check, a top-of-chain key-dispatch branch, the `Ctrl+H` entry, and NVS init at boot. The keyboard boundary held — chess consumes only `a–h`, `1–8`, and backspace; everything else hands control back to the player. State persists via NVS under its own namespace.

Two deviations from Intent worth noting:

- **Engine.** Original plan called for micro-Max. Licence check came back empty (no statement on author's page or in source header), so the PoC ships with a homegrown legal move generator and a random-legal-move CPU. Real strength is a follow-up.
- **Persistence.** Intent guessed SD; landed on NVS — tiny payload, no mount/removal failure modes.

Iteration from here moves to normal Formal/Explore changes. First candidates: a real CPU (minimax + material eval), cursor-based move entry instead of typed coordinates, board orientation when playing black.

**Changelog entry:**

```
## 0.22.0 — 2026-05-25

- Chess game added as a side activity. `Ctrl+H` enters; type moves in `e2e4` notation; any unconsumed key exits with state preserved. CPU plays a random legal move (PoC; real engine to follow). Board state persists in NVS under its own namespace and survives reboots. Music continues playing while chess is on screen.
```

**Documentation impact:** `map.md` has no chess node — needs catch-up under the Application root.
