# Changelog

## 0.12.0 — 2026-04-27

- Browser updates no longer flash. All UI now renders into an off-screen back buffer and pushes to the panel in one operation, so pixels transition directly from the previous frame to the next without an intervening black fill.

## 0.11.4 — 2026-04-27

- Diagnostics row overhauled to surface device resources at a glance: stack peak, ring-buffer headroom, RAM use, and underrun count as numeric percentages, plus a per-core CPU graph (cyan core 0 / orange core 1) updated four times a second with ~17 s of history. CPU readings self-calibrate after one mostly-idle second per core.

## 0.10.1 — 2026-04-27

- Browser gains a scrollbar at the right edge of the directory column — thumb shows position and visible range, hidden when content fits. Holding `;` / `.` now auto-repeats: the first press fires immediately, then a short delay, then steady stepping while held.

## 0.9.0 — 2026-04-26

- Footer redesigned as a single compact line: the playing track's name marquee-scrolls when too long for its slot, the progress bar carries play/pause state through its colour (slate-blue while playing, mid-grey while paused or stopped), and a teal volume bar sits at the right. Footer height halved from 16 px to 10 px, so the browser gains 6 px of vertical space. The hairline above the footer picks up the same slate-blue, framing the strip as a distinct region.

## 0.8.0 — 2026-04-26

- New keybindings: `+` / `_` for font size, `` ` `` to hide the diagnostics row, `\` to toggle filename wrapping, `?` for an in-app help screen, `'` to jump back to the currently-playing track, and Fn+`,` / Fn+`/` for previous / next track within the playing folder.
- Quieter browser look — no more printer-bar zebra, hairline rules between entries and around the browser frame, inverted-row selection in place of an outline, and slate-blue file names distinguishing them from cyan-blue directories. The first entry off the bottom partially renders to signal "scroll for more"; long names clip at the column edge instead of truncating with `...`.
- Diagnostics row redesigned as labelled proportion bars instead of numeric readouts; ring-buffer headroom bar narrower and grey. Volume rendered as a bar in the footer.
- Larger default font; the smaller font remains available via `_`. Filename wrapping is on by default but never wraps in the narrow preview column.

## 0.7.0 — 2026-04-26

- M4A audio playback. DRM-free .m4a and .mp4 files (AAC inside an MP4 container) now play alongside WAV / MP3 / FLAC / AAC, including seek and end-of-track advance.

## 0.6.0 — 2026-04-26

- Battery indicator now reflects the real charge level — calibrated to the loaded voltage range seen on this device, so a charged cell shows green rather than sticking in the blue 40–80% band. Cell voltage is also shown numerically in the diagnostics row.
- New emergency shutdown when the cell hits empty: playback stops, a "Battery Empty" warning shows for 10 seconds, and the device enters deep sleep to protect the cell. Resume by plugging in USB with the power switch on, then power-cycling.

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
