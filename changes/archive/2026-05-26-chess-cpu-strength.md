# Chess CPU strength

**Mode:** Explore

## Intent

The current CPU plays a uniformly random legal move — a placeholder. The user wants a CPU that actually presents a challenge, suitable for casual play on the Cardputer. This change is exploratory: survey the options for a stronger engine (algorithm family, evaluation depth, memory/CPU cost on ESP32-S3, complexity to implement and maintain) and pick one to build.

## Approach

The survey is the work — final algorithm choice is the output of Build, not a decision to pre-bake here. What the Approach does fix is the axes the survey covers, the constraints any candidate must satisfy, and how a candidate gets picked.

### Axes to compare

Each candidate is characterised on the same set of axes so they're directly comparable rather than each pitched on its own terms:

- **Algorithm family** — e.g. greedy 1-ply, alpha-beta with material + piece-square tables (PST), iterative deepening, MTD(f), MCTS, opening-book overlay.
- **Strength** — rough estimated Elo or qualitative ("beats a random move 95% of the time", "plays sensible openings", "occasionally finds 2-move tactics").
- **Per-move CPU budget** — milliseconds at a depth that meets the strength target on ESP32-S3 (240 MHz, dual core).
- **RAM footprint** — both code and runtime (transposition tables in particular dominate).
- **Code complexity** — lines of code, whether it's a port of an existing micro-engine (micro-Max, sunfish-style) or hand-written, and how the existing legal-move generator in `chess.cpp` slots in.
- **Determinism** — does it always play the same move from the same position, and does that matter?

### Hard constraints

These are non-negotiable and rule candidates out at the survey stage:

- **Audio task stays clean.** Chess CPU must not starve audio. Either the CPU work yields back to the main loop within an audio period, or it runs on the second core / a lower-priority task that gets pre-empted. This shapes which algorithms are viable.
- **Bounded per-move time.** A think time the user wouldn't notice in casual play — a budget number is set in the Unresolved list, but "engine pegs the CPU for 10 seconds" is out.
- **Fits the device.** Code and runtime within what's left on ESP32-S3 after audio + UI. NNUE-class evaluators don't qualify; small alpha-beta engines do.

### Strength and time targets

The survey is anchored against a **coached-beginner** strength target (roughly 1000–1400 Elo: rarely hangs pieces, sees one-move threats, handles basic tactics) and a **per-move time budget of up to 5 seconds**. Candidates that can't plausibly clear the strength bar within the budget on ESP32-S3 are out; candidates that wildly over-shoot strength at the cost of complexity are out too — this is a casual side activity, not a study tool.

### Engine origin

The survey is biased toward **writing the engine fresh on top of the existing legal-move generator** in `chess.cpp` rather than porting a micro-engine (micro-Max et al). The legal-move generator is already in place and matches the project's style; ports of dense public-domain engines tend to be readable only by their authors and would be a maintenance liability. A port is only revisited if hand-writing demonstrably can't hit the strength target inside the budget.

### Opening book

Out of scope for this change. At coached-beginner strength a competent search should produce sensible opening play without a book, and adding one couples flash storage and engine code to a separate concern. A bundled book can ride a follow-up change cheaply if real play shows the engine flailing out of book theory.

### Picking the winner

Build's done-when is "one candidate chosen, justified against the axes, ready for a follow-up implementation change". The choice itself is the deliverable — implementation happens in a separate change so this one doesn't bloat.

### Map

The [Chess](#chess) node currently flags the CPU as a "random legal move (PoC — placeholder for a real engine)". Map catch-up sits with the implementation change, not this one — the engine isn't built yet.

## Plan

**Topics**

- Survey candidate algorithm families against the axes — at minimum: greedy 1-ply material, alpha-beta with material + PST at a few depths, iterative deepening within the time budget, MCTS. Note quiescence search and transposition tables as orthogonal add-ons.
- For each surviving candidate, estimate per-move CPU and RAM on ESP32-S3 — back-of-envelope from branching factor × depth × per-node cost, plus any tables (PST, transposition).
- Check the audio-task constraint for each candidate — whether the search yields cleanly inside the main loop, or has to run on the second core / a lower-priority task.
- Sketch how the chosen candidate plugs into the existing legal-move generator and `cpuMove()` call site in `chess.cpp` — enough detail to confirm the surface area is small.
- Pick one candidate. Record the choice and the reasoning against the axes in the change document.

**Done when**

A single engine choice is recorded in this document with a one-paragraph justification referencing the axes, ready for a follow-up implementation change to act on.

## Survey

### Cost baseline

The existing legal-move generator in `chess.cpp` does king-safety, so a "node" in any search means generate-legal-moves + make/unmake. Average branching factor in middle game is ~35. A conservative per-node cost on ESP32-S3 at 240 MHz is ~300–500 µs (rough order, dominated by the move generator). All depth estimates below use 35 branching and 400 µs/node before pruning, then apply realistic alpha-beta reduction.

Audio runs on its own pinned core (`AUDIO_TASK_CORE` in `main.cpp`), so a long blocking search on the Arduino loop core doesn't starve audio. This removes the "must yield" constraint from candidate selection — a 5 s blocking search is acceptable; only UI freezes for the duration, and the `chess::setRedrawCallback` hook from [[chess-turn-indicator]] already flushes a "thinking" frame before the search begins.

### Candidates

**Greedy 1-ply material.** One MoveGen pass + pick the maximal material capture. ~500 µs per move. Strength ~600 Elo: takes free pieces, plays nothing else. Falls short of the coached-beginner bar. **Rejected.**

**Alpha-beta, material-only eval.** With reasonable move ordering, alpha-beta cuts the 35^d tree toward 35^(d/2). At depth 3 → ~5 k effective nodes → ~2 s; at depth 4 → ~30 k → ~12 s (over budget). Strength at depth 3 ~1100 Elo: sees one-move tactics, hangs pieces to two-move tactics. Within reach of the bar but no room for the horizon effect. **Kept as the floor.**

**Alpha-beta + material + piece-square tables (PST).** PST is a 6 × 64 int8 table = 384 bytes. Adds positional sense (knights to centre, pawns advance, king to back rank in middle game) for essentially zero search cost. Same depth, ~+200 Elo. ~1300 Elo at depth 3, comfortably inside the budget. **Kept.**

**Iterative deepening over alpha-beta + PST.** Search at depth 1, 2, 3, … checking the wall clock between depths; return the deepest completed result. Costs almost nothing extra (the lower depths are tiny) and lets the same code adapt automatically to position complexity — endgame positions with fewer legal moves reach depth 5–6 inside 5 s. Also gives natural move ordering (try last iteration's best move first) which sharpens alpha-beta cuts. **Kept.**

**Quiescence search.** At the search horizon, extend further along captures only until the position is "quiet" (no hanging exchange). Fixes the horizon effect — without it, a depth-3 search that ends on a winning-looking capture will play it even if the recapture wins material back. Tactically the single biggest cheap upgrade: maybe +200–400 Elo. Costs perhaps 30–50 % more nodes per search. Still fits the budget at depth 3 with iterative deepening. **Kept.**

**Transposition table.** A 32 KB hash with ~1 k entries speeds searches 2–3× by reusing seen positions. Real value only at depths where transpositions repeat (≥4); marginal at the depth 3 floor. Adds Zobrist hashing, replacement policy, and a tuning surface. **Deferred** — orthogonal add-on, easy to retrofit if the engine needs to push deeper.

**MCTS.** Random-rollout MCTS is weak at chess without a learned policy — chess positions are too tactical for blind playouts. Adding a policy means a neural net, which is out of scope on this device. **Rejected — wrong tool for chess.**

### Chosen engine

**Iterative-deepening alpha-beta, evaluation = material + piece-square tables, with quiescence search at the horizon. No transposition table, no opening book in v1.**

Justification against the axes:

- **Strength:** estimated 1400–1600 Elo at the 5 s budget — hits the coached-beginner target with margin; rarely hangs pieces, finds short tactics, plays sensible openings without a book.
- **Per-move CPU:** time-bounded by construction. Iterative deepening returns the best move found before the 5 s ceiling; positions with fewer legal moves use less.
- **RAM:** PST ≈ 400 bytes; search stack proportional to depth (a few KB); no transposition table. Comfortable.
- **Code complexity:** ~250–400 lines on top of the existing legal-move generator. All pieces (alpha-beta, ID, quiescence, PST) are well-documented standard chess code, easy to write and review.
- **Audio task:** blocking search runs on the Arduino loop core; audio is on its own pinned core. No interaction.
- **Determinism:** deterministic given a position, which is desirable for debugging. A tiny randomisation among top-equal moves can be added later if play feels repetitive.

### Call-site shape

`cpuMove()` in `chess.cpp` is the only touch point. Today it calls `generateLegal()` and picks at random; the new body calls a `searchBestMove(g_pos, deadline_ms)` which itself uses the same `generateLegal` / `makeMove` / `unmakeMove` primitives the rest of the file already exposes. The redraw callback hook for the "thinking" indicator already exists. No other modules touched.

## Conclusion

Completed. No code shipped — Build's deliverable was the survey itself and the recorded engine choice. Map catch-up (CPU node still says "random legal move (PoC)") moves to the follow-up implementation change, where the description is actually true at the moment it's written.


