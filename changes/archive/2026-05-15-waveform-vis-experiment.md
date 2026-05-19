# Waveform visualisation experiment

**Mode:** Wander

## Intent

See what the device can do for live waveform visualisation. Triggered by a key combination during playback (e.g. `Ctrl+W`), replace the browser area with a scrolling waveform — header and footer stay as they are. A vertical playhead line sits inset ~20 % of the way in from the left edge of the waveform region. The horizontal axis represents on the order of 0.5 s of audio.

This is exploratory: the goal is to see whether it works, how it feels, and what the CPU/SPI cost is.

## Log

- 0.17.12: tapping `AudioOutputM5CardputerSpeaker::ConsumeSample` for live peak data. Per-column min/max stored in a 240-cell ring (one cell per pixel column); a column commits when ~`hertz/2/240` samples have been accumulated (≈ 0.5 s window at the current sample rate). The ring lives on the audio output instance so the audio task fills it without synchronisation overhead; the UI task reads it without locking — single-cell torn reads are visible at most as a one-column glitch, acceptable for visualisation.
- View toggles via `Ctrl+W`. Layout: oldest samples on the left, newest on the right; new audio enters at the right edge and existing columns shift left as the audio task commits more ring slots. A static playhead line sits at x = 20 % from the left in the directory cyan colour. Frames take the browse-mode slate-blue.
- Redraw at ~30 Hz from `pollWaveform`, partial-row push (`presentRows(browserY(), browserH())`) — same shape as the marquee fix so cost is bounded.
- 0.17.13: bumped redraw to ~60 Hz to address user-reported choppiness.
- 0.17.14: moved playhead to right side (`SCREEN_W − 35`) to align with audible-now given the ~70 ms tap-to-audible latency in the existing audio path.
- 0.17.15: instead of moving the playhead, shrank the time window so the right-of-playhead 80 % equals the actual look-ahead. Window collapsed from 0.5 s to ~87 ms.
- 0.17.16: deeper pre-buffer in the audio output. ~150 ms of pre-decoded samples held between `ConsumeSample` and the speaker queue, giving the decoder room to run further ahead. Tap stays at `ConsumeSample` (deepest available future). Look-ahead grows from ~70 ms to ~220 ms, so the waveform window is now ~275 ms (3 × the previous). Pre-buffer is a flat int16 ring; `shipBuffered()` is called each audio-task iteration to drain into the existing 3-slot _buf staging area and submit via `playRaw`. `hardFlush()` empties pre-buffer + stops speaker, called at the top of `stopPlayback` so user-initiated stop/skip remains instant. Natural track-end still drains naturally so the tail isn't cut.
- 0.17.17: tidy-up — make waveform-vs-other-views interactions consistent.
  - Letter keys now exit waveform before entering search (`enterSearch` clears `g_waveform_active`). Previously typing in waveform mode would silently activate search "underneath" the waveform overlay.
  - `Fn+\`` (esc) dismisses whichever overlay is on top: waveform first, then search.
  - `stopPlayback` clears the waveform — there's nothing live to show once playback stops.
- 0.17.18: fixed core-1 stuck at ~50 % after natural track end. Natural end goes through the audio task's own cleanup, not `stopPlayback`, so the 0.17.17 dismissal didn't fire. Added a self-dismiss check in `pollWaveform`: if `g_audio_active` is false, turn the viz off and redraw. Cheaper than threading the dismissal through every cleanup path.
- 0.17.19: instead of dismissing the waveform at track end, keep it on screen as a static snapshot and just skip redraws while nothing's changing. `pollWaveform` now caches the audio output's ring head between calls and skips the redraw if it hasn't advanced. Cost on a frozen waveform: essentially zero — only the tick-rate gate and a single load+compare per tick. Auto-dismiss removed; `stopPlayback`'s waveform dismissal also removed. Explicit dismissal still works via `Fn+\``, letter key, or `Ctrl+W`.

## Conclusion

`Ctrl+W` toggles a live waveform of upcoming audio in the browser area. Playhead is fixed at 20 % from the left and marks "currently audible"; the 80 % to its right is real decoded-but-not-yet-played audio. Window is ~275 ms. Stays on screen as a static snapshot when audio stops, at zero CPU cost.

The audio path gained a ~150 ms pre-buffer between the decoder and the speaker so the decoder can run that far ahead — this is what makes the playhead-at-20 % geometry physically meaningful. Stop and skip stay instant via `hardFlush`; natural track-to-track gap grows by ~150 ms while the pre-buffer drains. This is a real change to the audio path, not just viz plumbing.

**Changelog entry:**

> Live waveform visualisation. `Ctrl+W` while a track is playing replaces the browser area with a scrolling peak waveform of the upcoming audio, with a playhead line marking what's currently audible. Stays on screen as a static snapshot when playback ends. `Fn+\`` (esc), any letter key, or `Ctrl+W` again to dismiss.
>
> Audio path gained a ~150 ms pre-buffer so the decoder runs further ahead of the speaker (enabling the waveform's look-ahead). Stop and skip are still instant; natural track-to-track gap is slightly longer (~150 ms) as the pre-buffer drains.
