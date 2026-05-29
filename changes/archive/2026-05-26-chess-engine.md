# Chess engine

**Mode:** Formal

## Intent

Implement the engine chosen in `2026-05-26-chess-cpu-strength`: iterative-deepening alpha-beta on top of the existing legal-move generator, evaluation = material + piece-square tables, with quiescence search at the horizon. Replace the current random-move `cpuMove()` so chess actually plays at coached-beginner strength within a 5 s per-move budget. No transposition table, no opening book in this change — both are deferred.

## Approach

### File placement

Search code lives in `chess.cpp` alongside the existing engine primitives (generator, make/unmake, eval). The file is already the chess module, and the search reuses `generatePseudoLegal`, `isAttacked`, `makeMove`, `unmakeMove`, and `Undo` directly — splitting them across files would mean exposing internal types just to satisfy a structural rule. A clear `// --- Search ---` section header is enough.

### Negamax with alpha-beta

Standard negamax. One recursive function returns the side-to-move score; the caller flips sign at each level. Eval and search both operate in centipawns. Mate scores use a large constant (e.g. 30000) minus distance-to-mate so shorter mates outscore longer.

### Evaluation

`evaluate(p)` returns a centipawn score from white's POV. Material values: P=100, N=320, B=330, R=500, Q=900, K large (not used for material balance, just for mate detection). One 6×64 PST table per piece kind, mirrored vertically for black — modest middlegame-flavoured tables (knights to centre, pawns advance, king on back rank). No endgame transition in v1; coached-beginner play doesn't need a king-activity term yet.

### Move ordering

Alpha-beta lives or dies on move order. Per node, in order:

1. The principal-variation move from the previous iterative-deepening iteration, if any, played first.
2. Captures sorted by MVV-LVA (most-valuable victim minus least-valuable attacker), descending.
3. Quiet moves in generator order.

No killer-move or history heuristics in v1 — return on complexity is small at depth 3–4.

### Quiescence search

At depth 0, instead of returning `evaluate(p)` directly, run a quiescence search: stand-pat with the static eval, then explore captures only (MVV-LVA ordered) until the position is quiet. Standard fail-soft form. No delta pruning, no check evasions in v1.

### Iterative deepening with a time deadline

Search depth 1, 2, 3, … in sequence. After each completed depth the best move is remembered. Before starting the next depth, check the deadline; if exceeded, return the last completed depth's best. If a depth aborts mid-search (deadline expires during it), discard its partial result and return the previous depth's best. A hard cap (say depth 8) prevents runaway in trivial positions where each depth completes in microseconds.

Deadline granularity: a wall-clock check every 1024 nodes inside the search, propagated as an abort flag back up the recursion. Cheap, responsive enough for a 5 s budget.

### Move-list storage

Search allocates move lists from a fixed-size stack array per recursion frame (`Move moves[256]` — the absolute theoretical max for a legal position is 218). This avoids per-node `std::vector` heap traffic that would dominate per-node cost. The existing `handleKey` path keeps its `std::vector<Move>` — it runs once per keypress, not millions of times.

### Integration

`cpuMove()` in `chess.cpp` is rewritten: fire the redraw callback (existing "thinking" hook from [[chess-turn-indicator]]), call `searchBestMove(g_pos, SEARCH_BUDGET_MS)`, apply the returned move via the existing `makeMove`, then run the same game-result detection it does today. No other modules touched.

### Out of scope for v1

- Transposition table.
- Opening book.
- 50-move and threefold-repetition draw detection — game state doesn't track halfmove clock or position history yet, and the bar is "casual play at coached-beginner level", not tournament-correct draw claiming.
- Endgame eval transition.
- Difficulty levels (one fixed 5 s budget).

### Map

Map catch-up is deferred entirely — outside the scope of this change. The accumulated debt (cursor-input + engine) sits for a later pass.

## Plan

- [x] Add a `generateLegalFixed(p, out[], &count)` variant that writes into a caller-supplied stack array, leaving the existing `std::vector` overload for `handleKey`
- [x] Add piece-value table and per-piece-kind PSTs (white-oriented; mirror at lookup for black)
- [x] Add `evaluate(p)` returning centipawns from white's POV using material + PST
- [x] Add MVV-LVA scoring helper and an in-place ordering pass for a move array
- [x] Add `quiesce(p, alpha, beta, deadline)` — stand-pat + capture-only search, MVV-LVA ordered
- [x] Add `negamax(p, depth, alpha, beta, deadline)` calling `quiesce` at depth 0
- [x] Wire the previous-iteration PV move to the front of move ordering inside negamax
- [x] Add a node counter + 1024-node deadline check that propagates an abort flag up the recursion
- [x] Add `searchBestMove(p, budget_ms)` doing iterative deepening 1..8, returning last completed depth's best
- [x] Replace `cpuMove()` body with `searchBestMove` call; keep the existing redraw-callback and game-result tail
- [x] Bump version (patch) — 0.22.3 → 0.22.4

## Log

- Refactored the canonical `generatePseudoLegal` and `generateLegal` to fixed-array primary forms with `std::vector` wrappers, rather than duplicating the move-gen logic. The vector path now costs a 1 KB stack buffer + copy per call; only used by `handleKey` and `updateResult` so the overhead is invisible.
- Stack-frame budget concern (256 moves × ~16 plies = 16 KB on the ~8 KB Arduino loop stack) avoided by allocating the per-ply move arrays as a single static `g_move_stack[24][256]` in BSS (24 KB). Reflects in the build: RAM 24.5% → 30.3%.
- Build clean, no warnings.

## Conclusion

Shipped as planned. The largest deviation from the Approach text was the move-generation refactor: rather than adding a parallel fixed-array `generateLegalFixed` alongside the existing vector form, the canonical primitives became fixed-array and grew thin `std::vector` wrappers — single source of truth for the move generator, with the vector path absorbing a small per-call copy that's invisible at cold-path rates. The 256-moves-per-ply stack-allocation plan would have blown the Arduino loop stack at depth 8, so the per-ply arrays became a single static `g_move_stack[24][256]` (24 KB BSS) — same intent, safer realisation.

Difficulty levels surfaced mid-Build as a follow-up; captured as `chess-difficulty` proposal.

**Changelog entry:**

```
## 0.22.4 — 2026-05-26

- Chess CPU now actually plays. Iterative-deepening alpha-beta search with material + piece-square table evaluation and quiescence at the horizon, time-bounded to 5 s per move and depth-capped at 8. Targets coached-beginner strength (~1400–1600 Elo). The "thinking" indicator from 0.22.3 becomes meaningful — the CPU now spends real time before moving. RAM usage 24.5% → 30.3% (24 KB BSS for the per-ply move stack); flash unchanged in relative terms.
```

**Documentation impact:** the [Chess](#chess) node still says "random legal move (PoC — placeholder for a real engine)". Catch-up was deliberately deferred from this change (Approach), so the map debt now covers both this change and the deferred cursor-input update. Should be addressed in a single per-node negotiation pass later.

