# Continuous viz across jumps

**Mode:** Wander

## Intent

Currently a track skip (`{`/`}`, autoplay-next, or stop) clears both visualisation rings via `hardFlush → resetVisualisation`, and an intra-track seek (`1`-`0`, `[`/`]`) does the same via `seekToByte`. The display blanks, then new audio scrolls in from the right. Change to: keep the existing rendered content on screen, just let new audio scroll in naturally as it arrives. Pre-jump content scrolls off over the zoom-time window. Visually no interruption.

## Conclusion

`hardFlush` (audio_output_m5.h) no longer calls `resetVisualisation`. `seekToByte` (main.cpp) drops both the `resetVisualisation` call and the `g_viz_prev_us = 0` re-anchor. `stopPlayback` keeps the audio-path flush (instant audible cut on stop / skip) but no longer touches the visualisation cursor.

Result: jumps leave the previously-rendered heatmap / waveform content in place; new audio enters at the right edge and pre-jump content scrolls off naturally over the zoom-time window (4 s currently).

`resetVisualisation` is left in tree (unused) as a clean API for any future case where a true blank is wanted.