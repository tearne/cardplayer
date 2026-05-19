# Lazy-load fuzzy index

**Mode:** Formal

## Intent

Free up the ~42 KB of internal RAM the fuzzy-search index currently holds whenever search isn't active. Load filters and the unigram top-K table from SD when the user opens search; free them when they close it (either by selecting a result or exiting). The same boot-time auto-build (when files missing or card fingerprint differs) still happens — only the *RAM residency* becomes lazy.

The motivating problem is heap pressure on track start. Some files need most of the available free heap for libFLAC working buffers; the persistent fuzzy index makes the margin tighter than it needs to be.

## Approach

### State machine adds an `Active` state

Currently `FuzzyIndex::State` is `Idle / Building / Ready`, with `Ready` meaning "loaded in RAM". The new shape:

- **Idle** — no index files on SD (or never built).
- **Building** — background build in progress.
- **Ready** — files exist on SD, header verified, fingerprint matches. **Not loaded.**
- **Active** — filters and unigram table loaded in RAM. Queries serve.

`initAtBoot` no longer loads filters/unigram — it only validates the file headers and lands the system in `Ready`. The expensive RAM-resident state appears only when `activate()` is called.

### `activate()` / `deactivate()` API

Two new public functions on `FuzzyIndex`:

- `activate()` — synchronously reads the file body (filters + unigram table) from SD into RAM. Transitions `Ready → Active`. No-op when already active or when state is `Idle` / `Building`.
- `deactivate()` — frees the filter and unigram vectors. Transitions `Active → Ready`. Header stays cached so `pathCount()` keeps working.

### Entry/exit hooks in the search UI

- `enterSearch` calls `FuzzyIndex::activate()` before running the first query. The ~30 ms read overlaps with the user typing the first letter — synchronous, single-shot.
- `exitSearch` (every exit path — selecting a result, backspace-on-empty, `Fn+\``) calls `FuzzyIndex::deactivate()`.
- `searchRunQuery` defensively calls `activate()` too: if the user happens to be in search mode when a background build completes (`Ready` appears), the next query picks it up.

### Background rebuild stays unloaded

When `~` triggers a rebuild, the worker writes new files to SD and ends in `Ready`, not `Active`. The user has to open search to load the new content. Matches existing UX: rebuild is rare and the load cost has already been paid for at first search after the rebuild.

### `query()` / `lookupPath()` gate on `Active`

Both no-op (empty result / false) when state isn't `Active`. The search UI sees "(indexing…)" in the input row for everything other than `Active`, which is fine — `activate()` from `enterSearch` should have moved us into `Active` synchronously by the time the first query fires.

## Plan

### Tasks

- [x] Add `Active` to `FuzzyIndex::State`; keep `Ready` meaning "files on disk valid, not loaded"
- [x] Split the existing `pbLoadIfMatches` into `pbVerifyHeaderOnDisk` (reads + validates header only) and `pbLoadBody` (loads filters + unigram); cached header retained across deactivate
- [x] `initAtBoot` calls `pbVerifyHeaderOnDisk`; if it passes, state becomes `Ready` (no body load)
- [x] `FuzzyIndex::activate()` loads body, transitions `Ready → Active`
- [x] `FuzzyIndex::deactivate()` frees filter and unigram vectors, transitions `Active → Ready`
- [x] `query` / `lookupPath` gate on `Active`; `pathCount` works from cached header in either `Ready` or `Active`
- [x] `enterSearch` calls `activate()`; `exitSearch` calls `deactivate()`
- [x] `searchRunQuery` calls `activate()` defensively so a background build completing mid-search becomes usable on the next keystroke
- [x] Background build worker leaves state at `Ready`, not `Active`
- [x] Revert the audio pre-buffer shrink (3000 → 7000 samples) — heap headroom from lazy-load makes the original size safe again

## Log

- 0.17.24: all tasks landed. Built clean. Handing back for the user to feel the ~30 ms activation latency on first letter.
- User-confirmed: working well. Activation latency imperceptible in practice.

## Conclusion

Landed as planned, plus an in-build revert: the fuzzy index's ~42 KB body is now resident only between `enterSearch` and `exitSearch`, and the audio pre-buffer is restored to its original 7000 samples (full ~220 ms waveform look-ahead). The lazy-load gives more than enough persistent heap to make the larger pre-buffer safe.

**Changelog entry:**

> Fuzzy-search index now lazy-loaded — its ~42 KB of filters + lookup table only sit in RAM while a search is open. Frees that memory back to the heap for playback and decoder allocations. First letter pressed in search loads from SD (~30 ms, imperceptible); subsequent keystrokes serve from RAM as before. The waveform pre-buffer is restored to its original size now that the headroom permits it — full ~220 ms look-ahead is back.
