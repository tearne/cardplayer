# Changelog

## 0.21.60 — 2026-05-20

- RAM trims: spectrum and waveform ring buffers shrunk to 256 columns (from 480), speaker output staging cut from 3 to 2 ring slots, audio and visualisation task stacks tightened. Heap usage 86 % → 82 % (~13 KB freed). No user-visible behaviour changes.

## 0.21.59 — 2026-05-20

- Visualisation pipeline simplified: the per-frame heatmap / waveform composition now writes straight into the main canvas instead of into a dedicated 27 KB sprite that mirrored the same pixels. Frees ~22 KB of RAM (93 → 86 %) and eliminates a heap-fragmentation failure mode where a diagnostics-row toggle could blank the screen and pin a CPU core when free heap was low.

## 0.21.58 — 2026-05-20

- Visualisation no longer blanks on track jumps or intra-track seeks. The previously-rendered heatmap / waveform content stays on screen and scrolls off naturally as new audio arrives.

## 0.21.57 — 2026-05-20

- Visualisation tear minimised. Render task now driven by `esp_timer` at the empirically-calibrated panel scan period (~16780 µs ≈ 59.6 Hz), so pushes consistently land at the same scan phase = stationary tear. With 1 column per render at 4 s on screen, the tear shift drops to 1 pixel — essentially imperceptible. `Ctrl+T` toggles a vertical-bar test pattern for per-device panel-rate calibration. Diagnostics row updates restored during visualisation via a lightweight header-rows-only push.

## 0.21.40 — 2026-05-20

- Scroll smoothness rework. The visualisation overlay is now rendered by a dedicated FreeRTOS task driven by an `esp_timer` at microsecond precision (~60 Hz), independent of main-loop poll variance. A persistent sprite scrolls left by N columns per render and only repaints the newly-arrived rightmost columns, dropping compose work from ~9 ms to ~2 ms per frame. Time-window, audio col rate, and render granularity are now derived from a single `VIZ_ZOOM_SECONDS` knob. Residual is a slow left-to-right tear sweep at the difference between nominal 60 Hz and the actual panel scan — to be fine-tuned in a follow-up change.

## 0.21.11 — 2026-05-19

- Panel SPI clock raised 40 → 80 MHz, halving display push duration (~8.5 → ~4.5 ms measured). Modest visible improvement to heatmap tearing; main benefit is lower main-loop CPU cost during visualisation playback.

## 0.21.3 — 2026-05-19

- Waveform and heatmap overlays can now show simultaneously. `Ctrl+W` and `Ctrl+H` are independent toggles; both-on gives a stacked dual layout (waveform 2/5 on top, heatmap 3/5 below) time-aligned via a single shared wall-clock cursor. The waveform's previous look-ahead and playhead-at-20 % design is gone — both views now read audio up to the right edge.
- New "Auto heatmap" Settings toggle mirrors "Auto waveform"; either or both open at non-paused track start.
- Heatmap time window narrowed to ~2 s on screen.

## 0.20.27 — 2026-05-18

- `` ` `` now toggles the diagnostics row while a visualisation overlay (waveform or heatmap) is up, instead of dismissing the overlay. Diagnostics is a natural companion to a running visualisation; the previous behaviour treated it as an exit shortcut.

## 0.20.26 — 2026-05-18

- Scrolling spectrum heatmap overlay during playback (`Ctrl+H`). 256-pt real FFT of the audio is log-binned into 28 frequency bands and rendered as a viridis-coloured heatmap scrolling right-to-left at ~3 s on screen. Mutually exclusive with the waveform overlay (`Ctrl+W`); dismissed by the same non-transport keys. +6 dB/oct frequency tilt flattens music's natural low-end bias so all bands carry comparable colour range; partial (~110 ms) compensation against the FFT-tap-to-speaker look-ahead aligns the rightmost column closer to audible-now.

## 0.19.0 — 2026-05-17

- Waveform overlay rewired. Transport keys (`space`, `{`, `}`, `'`, `[`, `]`, `1`-`0`), volume (`-` / `=`) and brightness (`Fn+=` / `Fn+-`) now act on playback without dismissing the overlay — you can keep watching while you skip, seek, or change volume. Letter keys, `enter`, `Del`, `?`, `Ctrl+W`, `Fn+\`` and navigation keys still dismiss.

## 0.18.19 — 2026-05-16

- New Settings screen, opened with `?`. Every user-tunable behaviour shows up as a row with its current value at a glance. `;` / `.` move; `Enter` activates toggles and actions; `,` / `/` adjust toggle and numeric rows (auto-repeats while held); `` ` `` / `?` / any letter dismiss. Settings is yellow-themed to distinguish it from browser (blue) and search (green).
- New tunables, all persisted: hide non-audio files (default on, browser-only filter), volume max (cap on the live volume), auto-play next track (default on; previously a fixed behaviour), auto-show waveform on play, screen idle timeout (`15s` / `30s` / `1m` / `5m` / `off`; default `1m`), brightness (8 log-ish steps from 6 to 255; default 255).
- Volume scale bumped from 0–10 to 0–64 for finer adjustment; volume bar in the footer scales to the user's chosen cap rather than absolute hardware max. Default boot volume 16, default cap 16. Volume keys (`-`/`=`) now auto-repeat and work through the Settings overlay. Pause (space) also passes through.
- Brightness shortcut: `Fn+=` / `Fn+-` (and their shifted variants) step user brightness from anywhere. Brightness applies live with a 200 ms ramp.
- Screen-idle state machine simplified: at the timeout, the panel does a single 10-second linear fade to zero. Marquee, progress bar, waveform and diagnostics continue updating right up until the panel actually goes dark. The previous separate "dim then off" stages are gone.
- Diagnostics row swaps the `L` (largest-free) and `M` (peak-pressure) heap readouts for `to` (countdown until backlight-off fade fires) and `im` (last IMU motion delta in mg). Diagnostics text colour bumped from a dim 0x7BEF to a clearly-visible 0xC618.
- Browser selection toned to a recessive slate-blue (0x4978) so the selected row reads as filled-but-not-shouting against the brighter frame.
- Old help overlay removed as a top-level screen; the keymap reference now lives inside Settings under "Key reference".

## 0.17.44 — 2026-05-16

- Screen idle-blanking. The display dims to 25 % after a minute of no keypress or motion, fades fully off after two minutes. Pressing a key or picking up the device fades it back to full in ~1 s. Audio keeps playing throughout. Battery life noticeably extended during long listening sessions.

## 0.17.41 — 2026-05-16

- Resume playback at the saved position. After a power-cycle, the previously-playing track loads paused near where you left off (within ~10 s) — press space to continue. Position is saved every 10 s during playback alongside the rest of the persisted state.

## 0.17.38 — 2026-05-15

- Help screen redesigned: now uses the same font size as the browser, scrolls with `;` / `.`, and re-flows wrapped descriptions to align under the description column instead of back to the left margin. Section headers get a grey background and hairlines separate entries.

## 0.17.34 — 2026-05-15

- Fixed: selected-row highlight in the browser was invisible after the 8 bpp canvas change. The previous tint colour quantised to pure black under RGB332 packing, so the fill drew nothing. Replaced with a value that survives quantisation as a dim slate-blue.

## 0.17.32 — 2026-05-15

- Playback control cleanup. `Enter` on the currently-playing track is now a no-op (was: stop). Track-skip moves from `Fn+,` / `Fn+/` to plain `{` / `}`, adjacent to the held-seek keys. Progress bar in the footer now updates during seek and jump-to-tenth even when paused. Skipping tracks while paused keeps the pause active — the next track starts at zero, ready to resume on space.

## 0.17.30 — 2026-05-15

- Waveform overlay dismissal is now consistent: any keypress dismisses it and is consumed; the next press acts normally in the mode underneath. Previously, pressing a letter while the waveform was overlaid on a search would silently append to the query instead of dismissing the overlay.

## 0.17.28 — 2026-05-15

- Off-screen canvas dropped to 8 bpp (256-colour RGB332). Frees ~32 KB of internal RAM at the cost of mild colour quantisation across the UI. Combined with the lazy-load fuzzy index, internal-RAM headroom during playback is now roughly double what it was — comfortable margin for FLAC and other format decoder allocations.

## 0.17.25 — 2026-05-15

- Fuzzy-search index now lazy-loaded — its ~42 KB of filters + lookup table only sit in RAM while a search is open. Frees that memory back to the heap for playback and decoder allocations. First letter pressed in search loads from SD (~30 ms, imperceptible); subsequent keystrokes serve from RAM as before. The waveform pre-buffer is restored to its original size now that the headroom permits it — full ~220 ms look-ahead is back.

## 0.17.23 — 2026-05-15

- Fixed silent track-skip on some FLAC files whose decoder needed more heap than was free. Shrunk the audio pre-buffer to recover ~8 KB of internal RAM during decoder init. Waveform visualisation's look-ahead is slightly smaller in consequence. Also added diagnostic Serial output (`loop returned false ...`, libFLAC internal messages) for future audio-path debugging.

## 0.17.19 — 2026-05-15

- Live waveform visualisation. `Ctrl+W` while a track is playing replaces the browser area with a scrolling peak waveform of the upcoming audio, with a playhead line marking what's currently audible. Stays on screen as a static snapshot when playback ends. `Fn+\`` (esc), any letter key, or `Ctrl+W` again to dismiss.
- Audio path gained a ~150 ms pre-buffer so the decoder runs further ahead of the speaker (enabling the waveform's look-ahead). Stop and skip are still instant; natural track-to-track gap is slightly longer (~150 ms) as the pre-buffer drains.

## 0.17.11 — 2026-05-14

- Track-name marquee scroll now ~4× cheaper. Tick rate halved (so the scroll moves at half speed), and per-tick redraws push only the footer rows to the panel instead of the whole canvas. Core 1 load during playback dropped from ~25 % to ~6 % — frees duty-cycle for automatic light-sleep, expected to extend battery runtime meaningfully.

## 0.17.8 — 2026-05-14

- Frame elements (top/bottom separators around the main area and the scrollbar) now take the browse-mode or search-mode accent colour — slate-blue when browsing, slate-green when searching.

## 0.17.7 — 2026-05-14

- Browser fuzzy search added. Type any letter in the browser to enter search; results refine per keystroke from an SD-resident index. `Fn+\`` (esc) or backspace-on-empty to exit. `~` rebuilds the index. The index is built in the background at boot (~28 s for ~3 K tracks) and reused across boots unless the card changes. Ranks by contiguous-substring strength against the filename only.

## 0.16.14 — 2026-05-10

- Internal: fuzzy track-finding investigation harness behind the `FUZZY_HARNESS` build flag, in a separate `cardputer-fuzzy` PlatformIO env. Default builds carry no cost. Measured viability of an SD-resident page-bloom + unigram-table index across the realistic library range; verdict and numbers in `changes/archive/2026-05-10-fuzzy-track-finding.md`.

## 0.16.3 — 2026-05-10

- Header rearranged: battery + voltage on the top-left, version/path on the top-right. With diagnostics showing, the CPU sparkline now uses the full header height — lines overlay the path region under high CPU, leaving it clean at normal load.

## 0.16.2 — 2026-05-09

- Diagnostics row (toggle `` ` ``) gets a taller CPU graph and two new heap readouts: `L` (largest free contiguous block) and `M` (minimum-ever-free, peak pressure mark). Browser loses ~2 entries when the row is shown.

## 0.16.1 — 2026-05-09

- Holding `Fn` no longer falls through to plain key actions on keys with no Fn binding. Track-skip `Fn+,` / `Fn+/` unchanged.

## 0.16.0 — 2026-05-09

- Settings now persist across power cycles — volume, current folder + cursor, font size, wrap-names, diagnostics-row visibility, and the playing track. The device boots back into the saved track loaded paused (press space to resume); a missing folder or track falls back gracefully. Press `Del` to bring up a confirmation prompt that wipes the saved settings and returns the device to defaults. The diagnostics row now defaults to hidden; toggle with `` ` ``.

## 0.15.0 — 2026-05-08

- Browser redesigned around responsiveness. The directory listing now uses the full display width (the preview panel is gone), the selected row gets a subtle dark blue-grey background tint instead of an inverted highlight, and a path breadcrumb in the header replaces the version string after the first keypress to keep you oriented. Cursor moves push only the changed row pair to the panel rather than the whole canvas, so scrolling stays smooth even through directories with hundreds of entries. Colours updated: directories bright cyan, audio files light grey, unplayable files dim grey.

## 0.14.3 — 2026-05-08

- Diagnostic memory taps removed from serial output — boot and steady-state are now quiet. Internal: `CONFIG_ESP_IPC_TASK_STACK_SIZE` raised from the framework default 1024 to 2048 to give comfortable headroom for an ESP-IDF system task that was peaking at ~976 bytes during boot. The 1024 default was hairline-marginal and would intermittently trip a stack-canary panic on this firmware/hardware combination.

## 0.14.2 — 2026-05-07

- Audio decode now runs on core 0 with the UI on core 1; the per-core CPU readouts in the diagnostics row are honest about the split. Previously both ran on core 1 (the cyan core 0 line stayed flat at 0%). The task watchdog now meaningfully protects the audio decoder.

## 0.14.0 — 2026-05-06

- FLAC playback no longer underruns, and multi-format track-change chains no longer crash — two regressions that arrived with the v3 framework migration are now fixed. Behind the scenes: the audio library swap from that migration is reversed (back to `ESP8266Audio` + the in-house M4A demuxer) while keeping the v3 platform and calibration-free CPU readings. ~33 KB of internal RAM and ~540 KB of flash are freed up at boot. HE-AAC M4A files (typical iTunes Store downloads) remain unsupported on this hardware and need re-encoding to LC-AAC or MP3 to play.

## 0.13.30 — 2026-04-28

- Internal: Arduino-ESP32 framework now rebuilds cleanly with custom sdkconfig overrides. `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` is now explicit (guaranteeing calibration-free CPU diagnostics regardless of upstream defaults), and a small pre-build patch script auto-applies an upstream-pending workaround so future config flags can be added without breaking the build.

## 0.13.15 — 2026-04-28

- Moved to Arduino-ESP32 v3 and the actively-maintained `arduino-audio-tools` audio library, retiring the in-house M4A demuxer. CPU readings in the diagnostics row now show real microsecond-accurate values from the first sample — no warmup before the numbers stabilise. Known regressions tracked as separate work: HE-AAC M4A files (typical iTunes Store downloads) currently fail to play, and FLAC has occasional underruns.

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
