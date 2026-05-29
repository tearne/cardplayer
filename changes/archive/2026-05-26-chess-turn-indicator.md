# Chess turn indicator

**Mode:** Formal

## Intent

Remove the turn indicator from the Chess screen. The CPU player responds effectively instantly, so a persistent "whose turn" readout carries no information the user couldn't already infer. Replace it with a brief spinner in the top-right corner shown only while the CPU is thinking — present long enough to confirm activity if a move ever does take noticeable time, absent otherwise.

## Approach

### Drop the turn label/value pair

The "turn" label and `white`/`black` value in the side panel (`src/chess.cpp` render, around y=18/28) come out entirely. The remaining panel rows (`last`, `input`, result/CHECK, footer) keep their current positions — no reflow, since the freed space sits at the top of the panel.

### Spinner during CPU move

Mirror the fuzzy-index spinner already on the header strip (`src/main.cpp:967`): same `|/-\` glyph set, same 150 ms phase cadence advanced by a poll function, single character cell. Position is the top-right of the side panel — flush with the panel's right edge, level with the `CHESS` heading on the left.

A "CPU thinking" flag gates visibility: set before invoking the CPU move, cleared after. With the current random-move CPU the flag flips inside one frame and the spinner flashes briefly; whenever the engine becomes slow enough to yield back to the main loop, the same flag plus phase advance animates the glyph naturally.

### Map

The [Chess](#chess) node mentions `e2e4`-style typed input (a separate open change is reworking that) but no turn indicator, so the conceptual description doesn't need touching for this change. Map catch-up — if any — rides with the cursor-input change.

## Plan

- [x] Remove `turn` label and value from the chess side-panel render
- [x] Add a `g_cpu_thinking` flag in chess
- [x] Set the flag before the CPU move, clear after, with a render-and-push around the call so the indicator is visible
- [x] Draw a static "thinking" label in the top-right of the side panel when the flag is set

## Log

- chess.cpp had no way to trigger a frame push from inside handleKey — the host's draw() only runs after handleKey returns. Added a small host callback (`chess::setRedrawCallback`) registered from main.cpp's setup; chess invokes it before running the CPU move.
- First pass accidentally reflowed the panel rows up to fill the freed space; reverted to keep `last`/`state`/etc at their existing y positions per the Approach's "no reflow" decision.
- Version bumped 0.22.1 → 0.22.2.
- Spinner replaced with static "thinking" label after user pushback that the animation was over-engineering. Phase / last-ms state dropped; the redraw callback stays (still needed to flush the frame before the synchronous CPU runs). Once an async CPU lands, the same label appears for the duration without further work. Version bumped again 0.22.2 → 0.22.3.

## Conclusion

Shipped narrower than the Approach: spinner-with-phase-advance discarded for a static "thinking" label after a mid-build conversation about whether animation earns its complexity. It doesn't here — the label's presence carries the signal at any think time, so the spinner state machine and a future poll-driven phase advance both fell away.

The host redraw callback (`chess::setRedrawCallback`) is the load-bearing piece of the change. It exists because chess.cpp can't reach the canvas / present path directly; without it the indicator frame would never flush before the synchronous CPU move. When the CPU eventually moves to its own task, the callback becomes the natural "wake up and redraw" hook for completion.

**Changelog entry:**

```
## 0.22.3 — 2026-05-26

- Chess: persistent turn indicator removed (the CPU replies instantly, so "whose turn" carried no information). Replaced with a static "thinking" label in the top-right of the side panel, shown only while the CPU is computing. Currently flashes for one frame because the engine is trivial; will become a meaningful indicator once a real engine lands.
```

**Documentation impact:** none — the [Chess](#chess) node didn't describe the turn indicator. The catch-up for the cursor-input change (which the user opted to skip) remains the only outstanding map debt.

