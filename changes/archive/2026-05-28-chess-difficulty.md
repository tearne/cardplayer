# Chess difficulty

**Mode:** Formal

## Intent

Expose a chess difficulty setting (Easy / Medium / Hard) so the user can play against weaker opponents when full strength isn't wanted. Levers from the v1 engine work: max search depth (the most direct strength knob), top-N root randomisation (variety + blunders on easy), and quiescence on/off (tactical blindness at the lowest level). One Settings row, not a raw time-budget exposure.

## Approach

### The three levels

Each level is a fixed tuple of three engine parameters:

| Level   | Max depth | Top-N root pick | Quiescence | Rough Elo |
|---------|-----------|-----------------|------------|-----------|
| Easy    | 2         | 3 (random)      | off        | ~700      |
| Medium  | 4         | 1 (best)        | on         | ~1200     |
| Hard    | 8         | 1 (best)        | on         | ~1500     |

Hard reproduces the current 0.22.4 behaviour exactly, so the default is Hard — no regression for anyone happy with the engine today. Easy is intentionally not "depth 1 random capture": it still searches two ply and orders sensibly, but picks randomly from the top three to keep games varied and prevent a fixed opening from always producing the same loss.

### Where the setting lives

The difficulty is chess-specific state, so it lives in the chess module and persists in the existing `chess` NVS namespace alongside board state. `chess.h` grows `Difficulty` enum + `getDifficulty()` / `setDifficulty()`. The Settings UI in `main.cpp` calls into those — main doesn't reach across the namespace into chess's internals.

The chess save/load path picks up the new key (defaulting to Hard on missing key, so existing installs roll forward without resetting).

### Settings row

A new `SR_CHESS_LEVEL` row of kind `SRK_NUMERIC` displaying a label (`Easy` / `Medium` / `Hard`) — same pattern as `SR_IDLE` which already shows enumerated labels through `IDLE_TIMEOUT_LABELS[]`. `,` / `/` step through the three values; `enter` does nothing on numeric rows by existing convention.

Row placement: in the numerics block, after `SR_IDLE`, before the action rows. Named `"Chess level"` to match the existing "Volume max" / "Font size" / "Brightness" style.

### Engine integration

`searchBestMove` and `negamax` read the current difficulty at the start of a search:

- `max_depth` replaces the existing `MAX_SEARCH_DEPTH` constant.
- A new top-N pick at root replaces the deterministic argmax with a uniform pick from the top-N scoring moves.
- Quiescence on/off: `negamax` at depth 0 returns `quiesce(...)` or `evaluate(...)` depending on the level.

`SEARCH_BUDGET_MS` stays a single 5 s constant — easy levels return well within budget naturally, no need to ratchet the time.

### Out of scope

- Per-level time budgets.
- Continuous-strength slider.
- Per-side asymmetric difficulty.
- Map catch-up — the [Chess](#chess) node's outstanding debt continues to sit.

## Plan

- [x] Add `Difficulty` enum (`EASY`/`MEDIUM`/`HARD`), `getDifficulty()` / `setDifficulty()` to `chess.h`
- [x] Add a level table in `chess.cpp` mapping each level to (max_depth, top_n, quiescence_on)
- [x] Persist difficulty in the existing `chess` NVS namespace; default to Hard on missing key
- [x] Plumb the level into `searchBestMove` — replace `MAX_SEARCH_DEPTH` constant with level's max_depth, branch on quiescence in `negamax` at depth 0
- [x] Implement top-N pick at the root: collect moves scoring within ε of the best, sample one via `esp_random`
- [x] Add `SR_CHESS_LEVEL` to the SettingsRowId enum and SETTINGS_ROWS array (numeric kind, label "Chess level")
- [x] Add `CHESS_LEVEL_LABELS[]` and extend `settingsRowNumStr` / `adjustSettingsRow` to handle the new row
- [x] Bump version (patch) 0.22.6 → 0.22.7

## Log

- `selectBestNext` shuffles `root_moves[]` in place during each iteration, which would have invalidated the top-N candidate set if the next iteration aborted mid-shuffle. Fixed by snapshotting `last_moves[]` + `last_scores[]` at the end of each completed iteration; top-N samples from the snapshot, not the live array.
- Difficulty loads independently of board state — a fresh install with no `board` key still picks up the default (HARD), and a corrupt board with a valid difficulty still applies the level.
- Build clean. RAM 30.3% unchanged (the new state is a handful of bytes); flash +804 bytes.
- 0.22.7 boot-loops on device. Backtrace decoded: `ipc1` task canary trips during `gpio_isr_register_on_core_static → esp_intr_alloc → heap_caps_malloc → multi_heap_malloc` at boot. The chain has nothing to do with chess code — it's the IPC task at the configured 2048-byte stack running out of headroom while installing a GPIO interrupt. Awaiting user direction on whether to bump `CONFIG_ESP_IPC_TASK_STACK_SIZE` (the standard fix) and whether 0.22.6 was clean (would indicate an indirect frame-size shift from my changes).
- User reports 0.22.6 also occasionally boot-looped, so this is creep, not a 0.22.7 regression. User wants the mechanism understood and the sustainability of the periodic-bump approach considered before acting. Investigation under way; notes follow inline.

### IPC investigation notes

**Historical peaks** (from `2026-05-08-internal-ram-optimisation.md` Log):

- Pre-bump (default 1024): peak ~976 (4.7 % headroom), then drifted to 1088 — over the limit.
- Post-bump (2048): probe showed `min_free=960`, i.e. peak ~1088. ~53 % headroom.
- 2026-05-28 (now, at 2048): peak exceeds 2048 → canary trip. So the peak has grown by **at least ~1 KB** since 2026-05-08.

That's a large jump for ~20 days of work. Worth understanding rather than just bumping.

**What the IPC task does.** ESP-IDF has one `ipcN` task per core. Code that *must* execute on a specific core (most commonly interrupt allocation, since interrupts are core-local) is marshalled through `esp_ipc_call`. The IPC task wakes, runs the function in its own stack, and returns. So the stack peak is whatever deepest call we marshal at boot.

**The faulting chain.** `gpio_isr_register_on_core_static → esp_intr_alloc → heap_caps_malloc → multi_heap_malloc`. This is GPIO ISR install. `esp_intr_alloc` is not itself stack-heavy, but `multi_heap_malloc` walks heap regions and on a fragmented or many-region heap can produce non-trivial frames.

**What changed since 2026-05-08.** No `platformio.ini`, `sdkconfig.defaults`, or library/framework changes — only `src/` files (main.cpp, audio_output_m5.h, chess.cpp/.h). Our code does not call `esp_intr_alloc`, `gpio_isr_register`, or `attachInterrupt` anywhere (grep clean). So the IPC-task work itself is the same system code calling the same system allocator — but the *heap state* and *binary layout* at the moment it runs are not the same.

Two plausible mechanisms, both consistent with the data:

1. **Heap-state drift.** A lot of static-RAM additions landed (canvas, viz buffers, fuzzy index, audio buffers — see `lots more RAM` and the ram-audit work). These shift where free blocks sit when ISR install runs, changing how far `multi_heap_malloc` walks. The walk depth shows up as stack.
2. **Stochastic worst-case.** The May 8 probe sampled one boot's peak (1088). With ~960 bytes spare it claimed "comfortable", but boot-time heap state is not deterministic across power-ups. The true worst-case could already have been close to 2048; recent changes nudged the distribution.

Either way, the relevant fact is: **the IPC task runs system code we don't own, on a stack we have to size against a worst-case we can't predict analytically**. There's no engineering route to a tight fit; only margin + measurement works.

**Sustainability options.**

- **(a) Bump + measure.** Increase `CONFIG_ESP_IPC_TASK_STACK_SIZE` to 4096, and add a one-shot diagnostic that reads `uxTaskGetStackHighWaterMark` for `ipc0`/`ipc1` post-boot and surfaces it (serial, or on the existing diag overlay). We see creep coming, rather than discovering it at boot-loop time.
- **(b) Bump alone.** 4096 now, accept that the next surprise will again be a boot-loop on hardware.
- **(c) Generous bump.** 8192. Roughly 6 KB permanent RAM overhead vs (a/b). Pushes the next bump out further.

RAM cost: the chip has ~512 KB SRAM. 2 KB extra (option a) is 0.4 %. 6 KB (option c) is 1.2 %. Neither is meaningful against the canvas/buffer pools we're already running.

Recommendation: **(a)**. The probe is the durable change — once we can see the peak, future bumps become data-driven instead of reactive. 4096 gives us immediate margin without commitment.

**Open question for the user.** When 0.22.6 "occasionally" boot-looped, was the symptom the same canary-on-`ipc1` panic, or just a non-starting boot? If the latter, there may be a second mechanism in play and this analysis only covers half of it.

## Conclusion

Three difficulty levels (Easy / Medium / Hard) selectable from Settings; Hard preserves the 0.22.4 engine behaviour exactly so the default is no-regression. Level persists in the existing `chess` NVS namespace.

Shipped as part of 0.22.8 alongside `ipc-stack-headroom` — the chess code in the working tree at the moment the IPC fix was tested became part of that flash, so this work doesn't get its own version bump. Extending the existing 0.22.8 changelog entry rather than adding a new one.

### Proposed changelog addition

Append to the existing 0.22.8 entry:

```
- Chess difficulty setting (Easy / Medium / Hard). Easy searches two ply and picks randomly from the top three scoring moves, with quiescence off — varied games and tactical blindness. Medium is depth 4 with best-move-only and quiescence on. Hard is the 0.22.4 engine unchanged (depth 8, best move, quiescence on). Settings row "Chess level"; default Hard so existing installs roll forward without resetting. Persisted in the chess NVS namespace.
```


