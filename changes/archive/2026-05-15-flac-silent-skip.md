# FLAC silent-skip bug

**Mode:** Wander

## Intent

Track down why certain FLAC files (e.g. `Battle_Illusion.flac` in /DJ/2024/a/) are skipped silently on the device, taking two subsequent tracks down with them. The file plays fine in VLC on a laptop, so the file itself isn't corrupt — failure mode is somewhere in the device's audio path. Currently the "loop returned false" branch in the audio task is silent, giving us no signal beyond the rapid-fire `playing:` prints with no `format:` line in between.

First step: add prints to identify whether `loop()` is failing on the first call or after some samples, and how many samples (if any) were consumed before the fail. Take it from there.

## Conclusion

Root cause: heap exhaustion. The failing tracks have FLAC `max_blocksize` that requires libFLAC to allocate working buffers larger than the heap headroom we had during track start. With the canvas (~65 KB) and fuzzy-index (~42 KB) resident, free heap was ~28 KB when the decoder began — below libFLAC's needs for these files. `process_single` returned false silently (no callback to `error_cb` for this OOM path).

Diagnostic path: added a `loop returned false` print with samples/srcPos/srcSize/heap, then redirected ESP8266Audio's `audioLogger` to Serial so libFLAC internal messages surface. Confirmed by shrinking the pre-buffer from 7000 to 3000 samples (recovers ~8 KB of internal RAM), at which point the failing tracks all play.

**Fix shipped**: pre-buffer reduced to 3000 samples (~68 ms at 44.1 kHz). Waveform look-ahead shrinks from ~220 ms to ~138 ms, window from ~275 ms to ~172 ms. Diagnostic prints retained — they're cheap and useful for future investigations. `audioLogger` redirect retained too.

**Memory pressure is the underlying issue**, not specific to FLAC. Headroom for decoder allocations is ~36 KB now, which is fine for typical CD-rip FLAC but marginal for hi-res. Follow-up changes will reclaim memory via lazy-loading the fuzzy index and dropping the canvas to 8 bpp.

**Changelog entry:**

> Fixed silent track-skip on some FLAC files whose decoder needed more heap than was free. Shrunk the audio pre-buffer to recover ~8 KB of internal RAM during decoder init. Waveform visualisation's look-ahead is slightly smaller in consequence. Also added diagnostic Serial output (`loop returned false ...`, libFLAC internal messages) for future audio-path debugging.
