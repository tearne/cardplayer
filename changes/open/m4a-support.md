# M4A support

## Intent

Add M4A playback alongside the existing WAV / MP3 / FLAC formats. M4A is the format that podcasts and most Apple-source audio commonly export, so its absence is the most likely missing-format surprise for someone loading a typical audio collection onto the SD card.

Scope is **audio-only, DRM-free** M4A files. iTunes-protected (FairPlay) files are explicitly out of scope.

Treated as exploratory: if the existing toolchain can be coaxed into decoding M4A without prohibitive cost, ship it; if not, the change closes as not feasible and we record what we learned in case we revisit later.

## Approach

### Re-use the existing AAC decoder; wrap M4A as an ADTS stream

ESP8266Audio's `AudioGeneratorAAC` (libhelix-aac) already decodes raw ADTS frames and is linked in for our existing `.aac` support. M4A is AAC samples inside an MP4 container, so the cheapest path is to leave the decoder alone and feed it a synthesised ADTS stream: parse the container once, then on each `read()` emit a 7-byte ADTS header and the raw sample bytes for the next frame. Switching to a different decoder library would be a much larger commitment for a single new format.

### `AudioFileSourceM4A` wraps `AudioFileSourceSD`

A new `AudioFileSource` subclass owning the underlying SD source. The constructor parses the MP4 boxes and builds the sample index; `read()` returns ADTS-prefixed bytes for the next sample; `seek()` snaps to chunk boundaries. All M4A-specific code lives inside this one class. `startPlayback()` decides whether to wrap based on file extension.

### Box parser is minimal — only what playback needs

Parse `ftyp` (sniff), `moov` → `trak` → `mdia/minf/stbl` → (`stsd/mp4a/esds`, `stsz`, `stco`/`co64`, `stsc`) for the audio track. Skip everything else. Reject files that aren't a single AAC audio track — multi-track, video, or protected — which covers the DRM-only scope boundary from Intent.

### Stream the sample-size table from disk

`stsz` for a long VBR file runs to hundreds of KB. Rather than enabling PSRAM — which adds always-on current draw and complicates keeping the audio hot-path inside fast internal RAM — keep a small window of entries in memory (say 256 entries, 1 KB) and refill from the SD card as the decoder advances. Sequential playback has perfect locality, so the window only refills every ~6 s of audio. Seeks recompute the window position from the target sample index and refill once. SD random-access latency (~1 ms) fits comfortably inside a frame interval (~23 ms at 44.1 kHz × 1024).

### Seek snaps to chunk boundaries

Each MP4 chunk groups ~100 samples (~2 s at typical bitrates); the chunk-offset table (`stco`/`co64`) fits comfortably in internal RAM. Sub-chunk seek precision isn't worth the complexity for this player.

### `makeGenerator()` learns about `.m4a` and `.mp4`

`isAudioName()` already lists `.m4a` and `.mp4` (so the browser shows them); `makeGenerator()` does not, so attempts to play them fail silently today — that's the visible gap. Add mappings for both extensions plus the source-wrapping in `startPlayback()`. `.mp4` shares the same code path; the wrapper rejects at parse time if the file isn't single-track AAC, which covers the video and protected cases. `.aac` continues to work unchanged (raw ADTS, no wrapper).

### Out of scope, deliberately deferred

- **Gapless playback.** AAC's encoder-delay and padding samples introduce ~50 ms of silence at start and end of each track. Audible only on truly gapless album playback, which isn't the player's primary use case. Revisit if it bites.

- **Map updates.** `map.md` is not touched as part of this change. The change is exploratory; if the build succeeds it returns to plan mode for a small follow-up that picks up the map node updates from the build's Log. If the implementation proves not feasible, no map work is wasted.

## Plan

- [ ] Create `src/AudioFileSourceM4A.{h,cpp}` with the class skeleton: subclass of `AudioFileSource`, owns an `AudioFileSourceSD*` for the underlying file, declares the `AudioFileSource` virtuals (`read`, `seek`, `close`, `isOpen`, `getSize`, `getPos`), and stores fields for parsed metadata — sample rate, channel count, AAC profile (AOT), sample-frequency-index, channel-config, total samples, chunk-offset table, sample-size window.

- [ ] Implement the MP4 box parser in the constructor. Walk `ftyp`, `moov` → `trak` → `mdia/minf/stbl` → (`stsd/mp4a/esds`, `stsz`, `stco`/`co64`, `stsc`); extract `AudioSpecificConfig` from `esds`. Skip everything else. Reject the file (leave the wrapper in a failed-init state, surfaced via `isOpen`) if it isn't a single-track AAC stream. Build the in-memory chunk-offset table.

- [ ] Implement the sample-size sliding window: a 1 KB buffer over the on-disk `stsz` table, refilled from the underlying source on demand. Provide an internal `sizeOf(sampleIndex)` accessor that handles refills transparently.

- [ ] Implement `read(buf, len)`: synthesise the 7-byte ADTS header from the cached AudioSpecificConfig plus the current sample size, then deliver the raw payload bytes from disk. Track position state across multiple `read()` calls within a single sample; advance to the next sample on payload exhaustion; report end-of-stream after the last sample.

- [ ] Implement `seek(pos, dir)`: translate a byte offset on the synthesised ADTS stream to a sample index, snap to the nearest chunk boundary, refill the size window, reposition the underlying source to the chunk's first sample. Implement the remaining `AudioFileSource` virtuals (`getSize`, `getPos`, `close`, `isOpen`) so the wrapper presents as a single synthesised ADTS-byte stream.

- [ ] Wire into `makeGenerator()` and `startPlayback()`. `makeGenerator()` returns `new AudioGeneratorAAC()` for `.m4a` and `.mp4` (in addition to the existing `.aac` case). `startPlayback()` wraps the underlying `AudioFileSourceSD*` in an `AudioFileSourceM4A*` for those two extensions before passing to `gen->begin()`. `.aac` keeps its current direct path.

- [ ] Test with the user-supplied M4A files: successful parse and decode without underruns; `[` / `]` seek and `1`–`0` percentage jumps land sensibly; track-end advance to the next file works as for the existing formats.
