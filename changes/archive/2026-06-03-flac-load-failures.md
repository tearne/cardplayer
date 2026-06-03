# FLAC files failing to load

**Mode:** Explore

## Intent

Certain FLAC files fail to start playing: the decoder reads the first block and produces no samples (`loop fail: samples=0` at `srcPos=8192`), so the player gives up and skips to the next track — which often fails the same way. Free heap sits around 26 KB at each failure, which points at the decoder being unable to allocate its working buffers. These appear to be particular (likely larger) files that played correctly in the past, so something has tightened the memory budget over time. The user wants these files to play again.

## Approach

### The failure is heap starvation in the FLAC frame-buffer allocation

libFLAC's `allocate_output_` mallocs, per channel, an output array plus a residual array sized to the frame's block size — roughly **8 × blocksize bytes per channel** (≈ 32 KB mono / 64 KB stereo at the common 4096 block size). With only ~26 KB free at play time, that allocation fails (`MEMORY_ALLOCATION_ERROR`), the decoder yields zero samples, and the track is dropped. Smaller-block / mono files squeak in; larger ones don't. *Reason: this pins the target — the fix has to leave enough contiguous heap for the largest realistic FLAC frame buffer, on the order of tens of KB.*

### Fix by reclaiming heap, not by touching the decoder

The vendored libFLAC stays as-is; the lever we control is how much heap is free when a track starts. *Reason: the buffer sizing is dictated by the file, not by us; modifying a vendored decoder is fragile.*

### Prime reclaim candidate: the fuzzy search index

The fuzzy index keeps library-sized vectors resident in heap (page filters, unigram hits) that matter only while searching, not during playback. Freeing them when not searching — reloading on demand — is the most promising way to hand the decoder its buffer. *Reason: likely the largest playback-time consumer that is safe to drop; to be confirmed by an on-device heap census.*

## Plan

**Topics**

- Confirm the mechanism on-device: log the decoder's error state, the required allocation size (block size × channels), and free heap, for both failing and working files.
- Heap census: log free heap at each boot milestone (after the canvas sprite, after SD + fuzzy index load, after audio init) and at play time, plus each file's required FLAC allocation. The deltas approximate the memory price of each subsystem — the closest practical stand-in for true per-feature accounting — and surface the largest reclaimable consumer.
- Choose and apply a reclaim strategy that reliably yields headroom above the largest expected FLAC frame buffer — freeing the fuzzy index during playback first, plus anything else the census flags.
- Validate against the real library.

**Done when** the previously-failing Discworld FLACs play to completion with a measured free-heap margin over the largest expected frame allocation, and search, visualisation, and alarms still work.

The first build step is the heap-census instrumentation — it needs a flash-and-report on hardware before any fix, since the reclaim target depends on the real numbers. No recalled "last working" version, so the approach is to engineer headroom guided by the census rather than bisect. Freeing the fuzzy index during playback (reloaded on next search) is accepted as the starting reclaim, pending what the census shows.

## Log

- Added `logHeap(tag)` printing free heap + largest contiguous block (`ESP.getMaxAllocHeap()` — fragmentation shows as largest << free). Boot milestones tagged `canvas` / `audio` / `sd` / `index` / `setup-done`; `play` logged as each track starts; the `loop fail` line now also prints `largest=`. Built clean at 0.25.7 — awaiting a hardware flash-and-report to read the census. The `index` milestone fires right after `initAtBoot`, which may kick a *background* rebuild, so steady-state index residency is better read from the `play` / `setup-done` figures.
- First on-device numbers (0.25.7): `play` is a clean **107668 free / ~98 KB largest** at first boot-resume, identical 106336 free on every subsequent track → **no leak**, `stopPlayback` fully reclaims. Failure lands at **~27 KB free / ~10–15 KB largest** having consumed ~80 KB. Even starting from a 98 KB contiguous block it fails, so the decoder *fragments* the heap as it allocates; a big block at the start isn't enough — total slack is what matters.
- **Correction: the fuzzy index is not the culprit.** It's lazy-loaded — `initAtBoot` only verifies; the large vectors (`s_pb_filters`, `s_pb_uni`) load on `activate()` (entering search) and free on `deactivate()`. So during playback the index body isn't resident; the ~106 KB free already excludes it. The original "free the index" reclaim plan is void.
- ~80 KB consumed before the first frame is consistent with a **stereo 4096-block** FLAC (output+residual ≈ 4 × 16 KB = 64 KB, plus source/bitreader/decoder ≈ 16 KB). Added instrumentation at 0.25.8 to confirm: the `loop fail` line now prints `flac ch/rate/bps` (via a `FlacGen` subclass exposing the protected STREAMINFO fields), and an on-demand `h` serial command prints a per-capability heap breakdown (default / internal / DMA) — works over USB-CDC without a reboot.
- 0.25.8 on-device: `flac ch/rate/bps` all read **0** — `AudioGeneratorFLAC` only sets those in its *write* callback (per decoded frame), which never runs on a failure, so the field is the wrong source; the real size comes from the frame header inside `allocate_output_`. The `h` breakdown showed `default == internal` (~106 KB) and DMA only ~7 KB lower with the *same* 69620 largest block → **no hidden reclaimable pool**; it's one internal heap, ~106 KB free but only ~68 KB largest contiguous.
- **Cleanup (0.25.10):** stripped the investigation instrumentation — boot/`play` `logHeap` milestones, the `logHeap` helper, the `FlacGen` subclass and the (misfired, always-zero) `flac ch=` failure line. Kept two low-cost diagnostics: `largest=` on the `loop fail:` line, and the on-demand `h` serial heap-census command. Builds clean.
- **Map:** with the user, promoted `Diagnostics` to a top-level node with two children — `On-screen Diagnostics` (the existing `Ctrl+D` header row) and `Console Diagnostics` (new — the `h` census and `largest=` field). Header re-pointed via See-also; `Ctrl+D` and Screen Idle references retargeted to the on-screen child.
- **Fix applied (0.25.9):** chess move stack is now heap-allocated in `chess::enter()` and freed in `chess::exit()` (a `Move(*)[MAX_MOVES]` pointer; `searchBestMove` falls back to the first legal move if the alloc ever fails). Entering chess with a track loaded shows a `ChessConfirm` modal ("Pause playback for chess?"); on confirm, `enterChessFreeingAudio` remembers the track + byte position, tears the decoder down (freeing its heap), then enters chess. `exitChess` frees the scratchpad first, then rebuilds the track **paused** at the saved position (no auto-resume — the user un-pauses). Mutual exclusion means audio's ~80 KB and chess's ~24 KB never coexist, so no added fragmentation. Verified in the `.elf`: `g_move_stack` static size dropped 24576 → 4 bytes. Builds clean; awaiting hardware test of the previously-failing files.
- **Root cause found via static `.elf` symbol census** (offline, `xtensa-...-nm --size-sort` on `.bss`/`.data`). Biggest static-DRAM consumers: `g_out` 35 KB (audio buffers + viz, needed), `rq_table` 32 KB (vendored decoder table in `.data`), **`chess::g_move_stack` 24 KB**, AAC `raac_*` tables ~30 KB and MP3 `hufftab*` ~25 KB (both vendored, sitting in `.data`/RAM), IMU config 8 KB. `chess::g_move_stack` = `Move[MAX_PLY 24][MAX_MOVES 256]` × 4 B = 24,576 B, permanently static even during audio playback. This is the regression: chess (added recently) claimed 24 KB of what used to be free heap, dropping the FLAC margin from ~130 KB to ~106 KB — onto the failing edge. Fix candidate: make the move stack resident only while chess is active rather than permanently static.

## Conclusion

The reclaim target turned out not to be the fuzzy index (the Approach's first guess — it's lazy-loaded and not resident during playback) but **chess's 24 KB move stack**, which was permanently static even while only audio was running. Found by a static `.elf` symbol census rather than on-device measurement — the offline census ended up being the most useful "memory price per feature" tool. Fixed by making the move stack heap-resident only while chess is open, and (per the user's design) pausing audio when entering chess via a confirm modal, so the audio and chess working sets never coexist; leaving chess reloads the track paused at its spot. Two serial diagnostics were kept (`h` heap census, `largest=` on load failures) and are now documented in the map under a restructured `Diagnostics` node. Shipped 0.25.10.
