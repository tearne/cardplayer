# Changelog

## 0.5.0 — 2026-04-25

- Within-track seek: hold `[` / `]` to scrub backward / forward (works while paused), or press a digit key to jump to that tenth (`1` = start, `5` = 40%, `0` = 90%).

## 0.4.0 — 2026-04-25

- Tracks now play through the folder automatically — when one finishes, the next audio file in filename order plays. Stops at the end of the folder; manual skip (`<` / `?`) still overrides.

## 0.3.2 — 2026-04-25

- Faster start for MP3 files with large embedded ID3v2 tags (typically album art).

## 0.3.1 — 2026-04-24

- Fixed garbled playback (loud high-pitched buzz) on mono MP3 files.

## 0.3.0 — 2026-04-24

- Audio decoding runs on its own FreeRTOS task on core 1, so directory scans and redraws no longer stutter playback.
- Header gains a second row with live diagnostics: audio task stack peak, decoder/speaker headroom bar, underrun counter.

## 0.2.0 — 2026-04-22

- Multi-column file browser with directory navigation. Arrow keys step in and out of folders; shifted arrows skip within the playing folder.
- Footer carries playback state (track name, play/pause, progress, volume) independently of where the user has browsed.
- Directories, audio files, and non-audio files distinguished by colour; alternating row tints inspired by green-bar printer paper.

## 0.1.0 — 2026-04-22

- Battery indicator in a new header strip at the top of the display.
- Version string shown at the left end of the header.
- First build under the new change-management process.

## Legacy (pre-0.1.0)

Proof-of-concept baseline inherited from the initial commit:

- List-based track browser of the microSD root directory.
- Playback of WAV, MP3, FLAC, AAC/M4A via ESP8266Audio through the ES8311 codec.
- Keyboard controls for selection, skip, play/stop, pause, and volume.
- Status line and progress bar at the bottom of the display.
