# Internal-RAM optimisation

## Intent

The Cardputer ADV has 320 KB of internal SRAM and **no PSRAM** (confirmed in `framework-rebuild-foundation`). The firmware is now competing with itself for that pool: track changes can OOM-reboot, M4A files with HE-AAC fail their decoder allocations, and any new feature that wants memory comes out of the same shrinking budget. Without PSRAM as an escape hatch, the only path to relief is reducing what the firmware actually uses internally.

The change is to take a careful look at where internal RAM is actually going — both the static, link-time picture and the live, on-device picture across the playback lifecycle — and pick reductions that move the needle. The aim is enough headroom to make M4A and track-change reliably, not just to play the easy formats.

## Approach

### Measure before reducing

Before pulling any lever, get the full memory picture so we know which reductions actually matter. Two views, both cheap:

- **Static / link-time:** the linker's per-archive size report and per-object breakdown shows what's nailed into `.bss` and `.data` at build time — the canvas, the audio output ring, the M5Cardputer / M5Unified state, the audio-tools globals (decoders, EncodedAudioStream, MultiDecoder), each decoder's static buffers.
- **Live / on-device:** free-internal-heap captured at boot, after a track starts playing, after a track-change between formats, during steady-state, near OOM. A small temporary diagnostic tap reading `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and the largest allocatable block lets us see the dynamic side directly.

The combination tells us, for each candidate reduction, whether the reduction would touch real bytes that matter at the times that matter. Without this we'd be guessing.

### Levers

Reductions are picked by data, one at a time, and judged on whether they make the failing cases work — not by isolated byte counts. Live candidates are tracked in **Options on the table**.

If measurement and the available levers don't close the gap, the audio-tools library itself becomes a candidate (alternatives: `schreibfaul1/ESP32-audioI2S`, low-level libhelix, esp-adf). Out of scope unless we hit the wall.

### Map

No map nodes affected by the internal reductions themselves. If the canvas ends up dropped, the **Browser** node may need its "no flicker" claim revised — per-node negotiation if so.

## Plan

This is exploratory work — pick a lever, measure, learn, pick the next. The static breakdown, the runtime tap, and lever 1 (lazy decoders) are already in place; the remaining gap is HE-AAC M4A SBR. What it takes to close that gap isn't yet known.

**Topics**

- Drive the M4A SBR OOM to ground via the avenues listed in **Options on the table**. One at a time, each choice informed by what the previous one taught us.
- Every avenue carries its own measurement step (using the existing tap, extended where needed) and its own validation against the success criteria before being declared done.
- Maintain **Options on the table** as exploration progresses: tried-and-ruled-out options drop out with a one-line rationale in Notes; new options surface as the picture sharpens.
- The reserved options (canvas drop, canvas shrink) stay reserved unless the preferred avenues all fail — they cost flicker-free rendering, which the change is explicitly trying to preserve.
- Final cleanup: remove the temporary memory-state tap and bump `APP_VERSION` (patch bump expected; revisit at Conclusion if scope grew).

**Done when**

- HE-AAC M4A files play without OOM during decoder init.
- Multi-format track-change between any of MP3 / FLAC / WAV / AAC / M4A doesn't reboot.
- Underrun counter stays at zero in normal use across all five formats.
- Diagnostic tap removed, version bumped.

## Notes

### 2026-04-28 — measurement summary

Static `.bss + .data` ≈ 71 KB. The dominant static cost in our code is the 18 KB `g_out` ring (6 × 1536 × int16). Decoder static state is tiny (60–744 B each); their `begin()` heap allocations are what cost — Helix MP3 ≈ 30 KB, Foxen FLAC ≈ 70 KB, Helix AAC ≈ 50 KB SBR. The 64 KB canvas is on heap, not static.

### 2026-04-28 — lever 1 (lazy decoders) shipped

Decoders + M4A container moved from global statics to on-demand construction in `startPlayback` / destruction in `stopPlayback`. Two problems fixed:

- **Foxen FLAC's `end()` retains ~64 KB** (`is_release_memory_on_end = false` by default). Full destruction of the decoder object releases it cleanly. `post_stop` free went 36 KB → 105 KB.
- **Track-change crash on 3rd FLAC swap** was a half-stitched-state bug: `begin()` after a partial-release `end()` left null internals. Fresh objects every play avoids it.

Peak across formats now matches peak of one format, not the sum. M4A `post_start` shows 91 KB free / 81 KB largest — but HE-AAC SBR still OOMs: it needs 50,788 B contiguous, and the M4A demuxer's `moov` sample-table parse fragments internal RAM further before SBR fires.

## Options on the table

Lever 1 (lazy decoders) shipped. Decision point now: HE-AAC M4A SBR still OOMs despite plenty of room at `post_start`. Avenues explicitly chosen *not* to lose the flicker-free canvas:

- **Diagnose first — fast memory tap during M4A startup.** Add a high-frequency (~50–100 ms) memory log during the first second or two of playback so we can watch free / largest evolve from `post_start` through the demuxer's box-parsing all the way up to the SBR allocation attempt. Tells us exactly how much the demuxer eats vs how much SBR needs, and which of the options below to pursue. **Picked as the next move; resume here tomorrow.**
- **Look at Helix AAC's SBR allocation.** Is the 50,788 B a single malloc or splittable? Is there a flag to disable / reduce SBR support (degrades HE-AAC to AAC-LC quality but keeps it playing)? Reading libhelix's source.
- **Look at `arduino-audio-tools`' M4A demuxer (`M4AAudioDemuxer` / `MP4Parser`).** It currently loads sample tables into RAM during the `moov` parse. If those can be streamed instead of fully buffered, the demuxer's footprint shrinks before SBR fires.
- **Try a different AAC backend.** `arduino-libfaad` and `arduino-fdk-aac` are alternatives. Memory profiles differ; one comparison build could tell us if SBR fits there.
- **Reduce other dynamic memory specifically when M4A is active.** Pre-decode ring (18 KB) and DMA buffer (16 KB) could be made smaller while M4A is the active format. Targeted, a bit hacky but local.

Reserved (still-viable but currently not preferred because they cost flicker-free):

- **Drop canvas back-buffer** (~64 KB) — would almost certainly fix M4A but loses flicker-free.
- **Shrink canvas to browser-body region** (~28 KB) — partial; might not be enough for SBR alone, plus loses flicker-free in the rest of the screen.

## Log

- 2026-04-29: Diagnostic taps narrowed the M4A failure to the `moov`-parse stage. The audio-tools demuxer eats ~45 KB during `stsz` parse (sample-size table — `uint16_t` × ~22,500 entries for an 8-min HE-AAC track), leaving SBR's 50,788 B contiguous request 12 KB short.
- 2026-04-29: Lever 2 (streaming `stsz`) shipped — see `src/m4a_stsz_buffer.h`. The audio-tools demuxer exposes `setSampleSizesBuffer()`; we plug in a `BaseBuffer<stsz_sample_size_t>` adapter that reads sample sizes directly from the open file with a 256-byte lookahead. Fully eliminates the 45 KB drop at moov parse.
- 2026-04-29: After lever 2, M4A still hit a libhelix 8 KB allocation failure during AAC `begin()` — heap going into AAC init was 91/82 KB free/largest; AAC's burst (PSInfoBase ~30 KB + SBR ~50 KB + buffers) carved the heap such that the trailing 8 KB request had nowhere to land.
- 2026-04-29: Lever 3 (trim audio output ring while M4A active) shipped. Ring moved from `.bss` to heap (was static `int16_t[6][1536]`); per-track `setRingDepth()` shrinks to 3 slots for M4A, restores to 6 for other formats. M4A now plays — heap goes 101 → 9.8 KB free in one burst, holds steady.
- 2026-04-29: **Blocker — multi-format track-change reliability regressed.** Crash inside Foxen FLAC during the second FLAC after an M4A → MP3 → FLAC → FLAC chain: `Guru Meditation Error: LoadProhibited`, `EXCVADDR=0x00000000`. Two contributing causes:
  1. **`setRingDepth` race condition.** M5.Speaker keeps a 2-deep wavinfo queue per channel; `playRaw()` returns but the speaker's DMA reads our buffer for ~70 ms after. `setRingDepth` calls `delete[] _buf` immediately on track change — corrupts memory the speaker is still reading from. Manifests later when an allocator hands the recycled region to a different caller.
  2. **No margin during FLAC after the chain.** `post_start` for FLAC after the chain shows 34 KB free / **15 KB largest contiguous**. Foxen's follow-on allocations during decode have no room.
- 2026-04-29: Stopping build, returning to plan mode for replanning. Race condition is fixable but doesn't address the underlying tightness; canvas drop was raised as the technically-right call, and library swap (`schreibfaul1/ESP32-audioI2S`) was raised as a wider option to test before committing.
