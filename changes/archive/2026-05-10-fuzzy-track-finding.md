# Fuzzy track finding

**Mode:** Explore

## Intent

Find out whether fuzzy matching across the full SD library is performant and robust enough on this device to be worth building on. The question is whether indexing, matching, and ranking can deliver responsive, predictable answers at realistic library sizes. A throwaway harness UI is fine; polished UI is out of scope.

## Approach

### Scale and stopping points

Realistic library: 500–2000 tracks; measurements scale beyond that to find the cliff. Per-keystroke latency is judged by feel, not a fixed ceiling. Index-build time is constrained by whether the build can run in the background while the UI stays responsive — if it can, the budget loosens considerably; if not, it has to be short enough that boot-time blocking is tolerable. Whether background build is achievable on this hardware is itself part of what the investigation answers. "Robust" means subsequence hits with sensible ranking; typo tolerance is a stretch goal.

### Memory is the headline constraint

No PSRAM — internal RAM only, already pressured by audio buffers. The first thing to nail down is the per-track footprint of any candidate index and whether the realistic library fits at all. If it doesn't, nothing else matters.

### Index built once, held in RAM — with SD fallback

First shape to measure: flat in-memory path index built once at boot. SD enumeration (`scanDir` at 25 MHz) is the slow part, so per-query SD walks are not interesting. If RAM proves too tight at realistic library sizes, the fallback is a precomputed index *file* on the SD card — n-gram or trigram so per-keystroke work is bounded reads, not a full scan. Worth measuring only if the in-RAM shape fails; named here so it isn't a surprise later.

### Match against path + filename

Full relative path, not just leaf, so directory tokens (artist/album) participate.

### Subsequence first

Start with fzf-style subsequence + simple ranking — cheapest credible algorithm. Escalate to edit-distance only if it proves insufficient on real queries.

### Measurement on device

Synthetic library sized past the 500–2000 realistic range to find the cliff. Record RAM footprint, cold-build time (foreground and background variants), steady-state per-keystroke time across query lengths, and worst-case (single-char queries matching everything). Harness is serial-driven — queries typed in the serial monitor, results printed back.

## Plan

### Topics

- Synthetic library: a reproducible way to populate the SD card with realistic-shaped paths at chosen sizes (500, 2000, well beyond), without manually filling the card.
- Serial harness: read query strings from the serial port, run a match, print the top results and timing back. Lives behind a build flag so it doesn't ship.
- In-RAM index: shape, per-track footprint, total footprint at scale, cold-build time from SD enumeration.
- Subsequence matcher with simple ranking (word-boundary and consecutive-match bonuses); per-keystroke timing across query lengths.
- Worst case: single-char and very short queries matching most of the library.
- Background build: attempt building the index on a FreeRTOS task while playback runs; observe whether playback stays clean and whether SD contention is a problem.
- SD-resident fallback: only if the in-RAM shape fails on memory — sketch and measure a precomputed n-gram index file. Skip otherwise.
- Version bump and changelog entry for the harness landing (kept behind a flag, not user-facing).

### Done when

A short written verdict — go, no-go, or "go with this fallback" — backed by measured numbers for footprint, build time, and per-keystroke latency at the scales tested. Enough information to decide whether to invest in real UI work.

## Log

- Version bumped 0.16.3 → 0.16.4 on entering build.
- Harness lands behind a `FUZZY_HARNESS` build flag, exposed as a separate `cardputer-fuzzy` PlatformIO env (extends `cardputer-adv`). The default `cardputer-adv` env compiles fuzzy_harness.cpp into a no-op TU and incurs no code/data cost — verified by separately building both envs clean.
- Index shape: one big `std::vector<char>` of null-terminated paths plus a parallel `std::vector<uint32_t>` of offsets. Per-track overhead beyond bytes-of-path is 5 B (4 B offset + 1 B null). Avoided `std::vector<std::string>` because of ~24 B per-entry header plus heap fragmentation.
- Matcher: fzf-style subsequence, case-insensitive, with consecutive-hit and word-boundary bonuses. Top-K kept in a min-heap so the per-query cost is `O(N · L_path) + O(N log K)`.
- Synthetic library generator writes `/SYNTH/<artist>/<album>/<NN word word.mp3>` 1-byte files, deterministic from a small word list. Spread is 6 albums × 12 tracks per artist (~72 tracks/artist). `rmsynth` tears the tree down.
- Background build: pinned to core 1 (UI core), priority 1 — keeps the audio task on core 0 untouched. Reads the result from a `volatile bool g_bg_done` flag with a non-volatile result struct (single-writer, single-reader, release-on-flag-store).
- Serial commands ready for measurement: `gen <N>`, `rmsynth`, `build`, `bgbuild`, `bgstatus`, `q <text>`, `bench`, `stats`, `mem`. Memory probe reports `MALLOC_CAP_INTERNAL` free / largest / minfree.
- All measurements are device-side — handing back to the user to run the harness over USB serial and feed the numbers back.
- Observed during measurement: while `gen` runs, on-screen diagnostics (CPU sparkline, RAM%) freeze. Cause: harness commands run synchronously inside `loop()` on core 1, starving `pollDiagnostics()` and the redraw. RAM also barely moves during `gen` — it's pure SD I/O with only transient `String` temporaries. Confirms that any synchronous index work will freeze the UI; background-build is load-bearing.
- Baseline at boot, no synthetic library yet: `mem free=152448 largest=143348 minfree=152376` — ~149 KB internal heap free, single largest block 140 KB.
- `gen 500` took 37.5 s (~75 ms/file). Pure SD I/O — not on the critical path for measurement; the relevant build-time number is the scan, not the generation.
- 0.16.5: first `bgbuild` run tripped the task watchdog and rebooted. Cause: bg task and the Arduino `loopTask` both priority 1 on core 1; without explicit yields, IDLE_1 starves and the task watchdog panics. Fix: `vTaskDelay(1)` every 32 directory entries inside `indexBuildScan`. Negligible cost relative to SD read latency.
- 0.16.6: foreground `build` aborted on /SYNTH (3-deep tree). Cause: the recursive scan held the parent directory's File open while recursing into a child, and FATFS on this platform supports only a small number of concurrent open handles — exhausted at depth ≥ 3. Refactored to drain the parent into a local `vector<String>` of subdir names, close the parent File, then recurse. At most one File open at any moment.
- 0.16.6 still aborted (same place). addr2line: `operator new` → `std::bad_alloc` thrown from `std::vector<char>::insert` reallocation, escapes to `__cxa_throw` → `std::terminate` (exceptions disabled), with 130 KB free heap reported a moment earlier. The transient occupancy during geometric growth at ~32 KB collides with internal-heap fragmentation.
- 0.16.7: pre-reserve `g_index_buf` (64 KB) and `g_index_offs` (2K entries) before scan, so growth re-allocations don't happen during the walk. Also flattened the recursion to an iterative worklist — same depth-first behaviour, but only the worklist vector grows, no nested File frames. Added `HARNESS: scan <dir>` print on each pop so we see exactly where the next abort (if any) occurs.
- 0.16.8: added per-8-directory mem probe inside the scan, revealing the actual cause. At scan[105], `paths=1042 buf=62225 cap=65536 free=51948 largest=39924`. The buf approaches the 64 KB reservation; the next `vector::insert` triggers a doubling-grow request for **128 KB**, but largest free is **40 KB** → `bad_alloc`. **Headline finding: `std::vector`'s doubling strategy is fundamentally incompatible with this device — once the vector reaches `largest_free / 2`, the next grow is unattainable.** The largest free contiguous internal block at boot is ~120 KB, so the absolute ceiling for an in-RAM index using single-allocation strategy is ~120 KB / ~60 B per path = ~2000 paths, exactly at the realistic upper bound. Real-world libraries with longer paths max out closer to ~1300 paths.
- The agent (incorrectly) suggested moving the index to PSRAM, citing `g_canvas.setPsram(true)` in main.cpp as evidence. The user pushed back; on inspection the board declares `maximum_ram_size: 327680` (320 KB internal only), `sdkconfig.defaults` has `# CONFIG_SPIRAM is not set`, and runtime free heap (~130 KB) is consistent with internal-only. The Approach's "no PSRAM" was correct; `setPsram(true)` is silently falling back to internal RAM. Proposed as a separate change `psram-misleading-code.md` to decide whether to remove or comment the misleading call.
- 0.16.9: implemented the SD-resident fallback per the Approach. `/FZTI.idx` file = 16-byte header + null-terminated path strings. `sdbuild` streams paths directly into the file as the SD walk visits them — never accumulates in RAM, sidestepping the in-RAM cliff. `sdquery` reads the file in 4 KB chunks, scoring null-terminated paths in place; carry buffer handles paths that straddle chunk boundaries. Top-K kept in a small heap. Per-query memory is ~5 KB constant regardless of library size. Commands: `sdbuild`, `sdq <text>`, `sdbench`.
- 0.16.9 measurement at 3141 paths (real DJ library + 500 synth): `sdbuild` 21 s, file 187 KB. `sdquery` ~200 ms across all query lengths, top-K results ranked sensibly. Memory steady-state unchanged (~130 KB free). Verdict: SD-resident shape works at scale — borderline-instant feel, viable for a search box. User asked for a faster path before settling.
- 0.16.10: implemented the n-gram speedup per the Approach. Page-bloom (PB) index — paths partitioned into pages of 64 consecutive entries; per page, a 4096-bit Bloom filter records which trigrams (3-char windows, case-folded) appear in that page's paths. Filters loaded into RAM at first query (~25 KB at 3 K paths). For queries ≥ 3 chars, each page's filter is tested against the query's trigram bits; only candidate pages are read from SD. Queries < 3 chars fall back to flat sdQuery. Stored at `/FZTI.pb`. Commands: `pbbuild`, `pbq <text>`, `pbbench`.
- 0.16.10 measurement at 3141 paths: `pbbuild` 214 ms, file 25 KB. `pbq saffrn` (6 chars, 4 trigrams): 72 ms with 10/50 pages scanned — 3× speedup. `pbq revem` (no matches): 1.5 ms — filter rejected all pages, ~130× speedup. `pbq ech` (3 chars, 1 trigram): 240 ms with 49/50 pages scanned — no speedup because per-page filter at 50 % saturation gives ~50 % false-match rate per query trigram, and a single-trigram query has nothing to compound. **Key insight: PB filter helps in proportion to (1/saturation)^n_trigrams. Effective for 5+ char queries; modest for 4-char; useless at 3-char and below.**
- 0.16.11 (PB v2): added per-character precomputed top-K table for instant 1-char queries. At build time, walk paths and for each distinct character that appears in a path, score the path against query=that_char (first-occurrence + word-boundary bonus + length tiebreak), keep top-K=16 per character. Stored as 256 × 16 × 4 B = 16 KB table appended to `/FZTI.pb`. At query time, 1-char queries become an O(1) RAM table lookup — zero SD reads. Bumped the PB header version to v2 so v1 files are flagged for rebuild. 2-char queries still fall back to flat sdQuery (bigram filters at our page size would saturate too far to be useful).
- 0.16.11 measurement: `pbq z` 79 µs (effectively instant), `pbq a` 34 ms (mostly first-time pb-load overhead — 42 KB read once). 1-char fastpath confirmed working.
- 0.16.12: stack overflow from a 4 KB-on-stack chunk buffer in `sdQuery`. The first time `sdQuery` was called via `pbQuery` (i.e. through the 2-char fallback rather than directly from `cmdSdQuery`), the deeper call chain pushed the loopTask's 8 KB stack into the heap region; the next `malloc` inside `SD.open` hit corrupted free-list metadata and panicked with EXCVADDR=0xffffffff. Diagnosed via decoded backtrace (heap_caps_malloc → strdup → VFSFileImpl). Fix: move the chunk buffer (and the equivalents in `sdLookupPath` and `pbQuery`) from stack to file-static. Single-threaded harness so static is safe. Saves ~9 KB of stack across the three functions and prevents future call-chain regressions.
- 0.16.12 introduced a 2× perf regression: the static buffers were aliased through pointer locals (`uint8_t* buf = s_buf;`), so `sizeof(buf)` evaluated to `sizeof(uint8_t*) = 4` instead of the array size, capping each SD read at 4 bytes. Fixed in 0.16.13 by referencing the static arrays directly (no aliasing).
- 0.16.13 measurement (PB v2 with stack fix): 1-char `z` 80 µs, 2-char `ab` 196 ms, 3-char `ech` 230 ms, 5-char `saffrn` 73 ms. Steady-state RAM cost ~50 KB. SD storage 230 KB total. Workable verdict for a real UI.
- 0.16.14: added `bgsdbuild` / `bgpbbuild` (and corresponding `bgsdstatus` / `bgpbstatus`) so we can confirm both builds run on the FreeRTOS task while audio plays. Same shape as the early `bgbuild` (priority 1, core 1, yields via `vTaskDelay(1)` every 32 entries).
- 0.16.14 measurement: `bgsdbuild` 28.6 s (vs 21 s foreground — ~35 % slower under contention), `bgpbbuild` 393 ms (vs 309 ms foreground). Audio playback stayed clean through both, including a track change mid-build. Memory low-water during background work was ~20 KB free internal — tight but adequate. Background path validated.

## Conclusion

**Verdict — go, with the SD-resident shape.** Fuzzy track-finding is viable on this device using the page-bloom + unigram-table approach measured here. The Approach's first shape (in-RAM flat index) is not viable at the realistic library size, so the fallback the Approach named becomes the recommended path.

**Headline numbers at 3141 paths (real DJ library + 500 synthetic):**

| Query length | Latency | Mechanism |
|---|---|---|
| 1 char | < 100 µs | Precomputed top-K table, RAM-only |
| 2 char | ~200 ms | Flat scan over SD index file |
| 3 char | ~230 ms | Page-bloom filter (~1/50 pages skipped) |
| 5+ char | ~70 ms | Page-bloom filter (~40/50 pages skipped) |
| No matches | ~1 ms | Page-bloom filter rejects all pages |

Steady-state RAM cost: ~50 KB. SD storage: ~230 KB total. Index build: ~21 s foreground (`sdbuild`) + ~0.3 s (`pbbuild`); ~28.6 s + 0.4 s when run on a background task while audio plays — audio remained clean, validating the background path.

**Deviations from the Plan.** Two unplanned additions:

- *Unigram top-K precompute (PB v2).* The Plan didn't anticipate a per-character fastpath; it emerged once the trigram-only filter showed the 1-char case as a hard floor at ~200 ms. A 16 KB precomputed table reduces 1-char queries to a RAM lookup. Bumped the page-bloom file format to v2.
- *Iterative-worklist scan and stack-resident chunk buffers.* The Plan implied recursive directory walking and stack-local buffers; both bit us in 0.16.6 (FATFS handle exhaustion at depth) and 0.16.12 (stack overflow on deeper call chains). Both fixes are noted in the Log for future reference.

**Documentation impact.**

- `map.md` — no new node yet; this was an investigation, not a feature. A real "Fuzzy Finder" node belongs with the UI work that follows.
- A separate proposal `changes/open/psram-misleading-code.md` was spawned during this work (the agent was misled into asserting PSRAM availability by `g_canvas.setPsram(true)` despite `CONFIG_SPIRAM` being unset).

**Harness disposition.** The `cardputer-fuzzy` env and `fuzzy_harness.cpp` land behind the `FUZZY_HARNESS` build flag; the default `cardputer-adv` env compiles the harness as a no-op TU and incurs no cost. Recommend leaving in place for the next iteration (real UI work will want to re-measure on the production index format).

**Proposed changelog entry (internal, not user-facing):**

> Internal: fuzzy track-finding investigation harness behind the `FUZZY_HARNESS` build flag, in a separate `cardputer-fuzzy` PlatformIO env. Default builds carry no cost. Measured viability of an SD-resident page-bloom + unigram-table index across the realistic library range; verdict and numbers in `changes/archive/`.
