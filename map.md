# Application

[Down](#main)
[Down](#settings)
[Down](#chess)
[Down](#standby-clock)
[Down](#playback)
[Down](#controls-and-navigation)
[Down](#diagnostics)
[Down](#screen-idle)
[Down](#persisted-state)

A minimal media player for MP3 and FLAC files on the M5Stack Cardputer. The tree below mirrors how the device is navigated — screens nest by how they are reached, and Esc backs out toward [Main](#main) — with cross-cutting subsystems (playback, input, persistence) as their own branches.

```
Application
├ Main
│ ├ Header
│ │ ├ Version
│ │ ├ Path Breadcrumb
│ │ └ Battery
│ │   └ Emergency Shutdown
│ ├ Footer
│ ├ Browser
│ │ ├ Fuzzy Search
│ │ └ Recent-track Trail
│ └ Visualisation
│   ├ Waveform
│   └ Spectrum
├ Settings (unreviewed)
│ ├ Key Reference
│ ├ Reset Confirmation
│ └ Alarms
│   ├ Set Current Time
│   └ Alarm Editor
│     ├ Days
│     └ Track Picker
├ Chess
├ Standby Clock
│ └ Alarm
├ Playback
│ └ Audio Formats
├ Controls and Navigation
├ Diagnostics
│ ├ On-screen Diagnostics
│ └ Console Diagnostics
├ Screen Idle (unreviewed)
└ Persisted State
  ├ Player State
  └ Write Model
```

# Main

[Up](#application)
[Down](#header)
[Down](#footer)
[Down](#browser)
[Down](#visualisation)

The home screen and the device's root. A [Header](#header) strip at the top, a [Footer](#footer) strip at the bottom, and a main area between them showing one of three contents: the [Browser](#browser) directory listing (default), [Fuzzy Search](#fuzzy-search), or the [Visualisation](#visualisation) overlay. The header and footer belong to Main — the full-screen screens ([Settings](#settings), [Chess](#chess), [Standby Clock](#standby-clock)) replace the whole display.

Esc (`` ` ``) on any other screen backs out one level toward Main; pressing it at Main, with nothing above to back out to, enters the [Standby Clock](#standby-clock). See [Controls and Navigation](#controls-and-navigation) for the full screen tree.

# Header

[Up](#main)
[Down](#version)
[Down](#path-breadcrumb)
[Down](#battery)

A strip at the top of the display, 42 px tall when the diagnostics row is shown, collapsing to 10 px when hidden. The first row carries the battery readout on the left and either the version string or the path breadcrumb on the right. The second row, when shown, carries the [On-screen Diagnostics](#on-screen-diagnostics) readouts; the CPU sparkline within them spans the full header height in its right-side region, drawing over the path/version slot when sparkline pixels reach that high.

While the fuzzy-search index is rebuilding (background, at boot or manually triggered), a one-character spinner cycles in the top-right cell and the version / breadcrumb slot shrinks by one char to make room.

**See also**

- [On-screen Diagnostics](#on-screen-diagnostics) — the optional second row this strip expands to show.

# Version

[Up](#header)

The application's semver version string, rendered at the left end of the header in small text. Visible from boot until the user's first keypress, after which the slot is repurposed for the path breadcrumb.

**Detail**

- Stored as an `APP_VERSION` constant in source; bumped by hand when cutting a build.

- Tracked alongside `CHANGELOG.md` at the project root.

# Path Breadcrumb

[Up](#header)

The current directory's full path, rendered in small text at the left end of the header in place of the version once the user has interacted with the device. Keeps the user oriented now that the browser is a single full-width column and no longer hints at directory contents via a preview.

**Detail**

- Static-trim with a `...` prefix when the path is wider than its slot — newest (rightmost) segments stay visible, older ones are dropped from the left. No animation, no per-frame cost.

- The slot runs from the left edge of the header to just before the voltage readout.

# Battery

[Up](#header)
[Down](#emergency-shutdown)

A small icon at the right end of the header showing remaining charge, with the cell voltage as text immediately to its left.

> [!IMPORTANT]
> No software-readable charging state on this hardware. The TP4057 charger is purely analog; its status pins are not routed to a GPIO. See `reference/notes.md`.

**Detail**

- Icon 30 px wide × 8 px tall. Voltage label `N.NNv` (2 dp) sits immediately to the left of the icon in the same row.

- Level rescaled from voltage: `clamp((mv − 3400) × 100 / (3800 − 3400), 0, 100)`. Bounds (`LOADED_EMPTY_MV`, `LOADED_FULL_MV`) calibrated to the loaded voltage range on this hardware — the M5 default 3.3–4.1 V map gives a stuck-blue ceiling because a charged cell sags below 4.1 V under load. Empty is set above the 3.0 V damage threshold for cell longevity.

- Fill colour by level: green / blue / yellow / red / bright-red bands. Polled every few seconds.

# Emergency Shutdown

[Up](#battery)

When the cell hits the empty cutoff, the device protects itself by powering off cleanly rather than letting the cell discharge further.

**Detail**

- Triggered when displayed level reaches 0 (voltage ≤ `LOADED_EMPTY_MV`).

- Playback stops, the screen blanks, and a centred warning shows for 10 seconds. Then `M5.Power.powerOff()` puts the ESP32 into deep sleep.

- Warning text: title "Battery Empty" in red, body "Charge with power switch ON" wrapped to two lines. All at double font size.

- One-way: with no charging signal available, plugging in USB during the warning window has no effect. Recovery is a physical power-cycle.

- On boot, if voltage is already at or below `CRITICAL_EMPTY_MV` (~3300 mV), skip the warning and sleep immediately — the cell has dropped past the warning band during a previous off-then-on cycle without charging, and further on-time would only stress it.

- Recovery: leave SW1 on, plug in USB. While in deep sleep the ESP32 draws ~10 µA, so TP4057 charges the cell at near-full rate. To resume use, press the boot button (BTN1) or briefly unplug-and-replug USB to trigger a hardware reset.

# Diagnostics

[Up](#application)
[Down](#on-screen-diagnostics)
[Down](#console-diagnostics)

Device-resource and memory readouts for development, surfaced two ways: [On-screen](#on-screen-diagnostics) as an optional header row, and over the [Console](#console-diagnostics) on the USB serial port.

# On-screen Diagnostics

[Up](#diagnostics)

> [!WARNING]
> Unreviewed.

Live device-resource readouts shown as the [Header](#header)'s second row. Hidden by default; toggle with `Ctrl+D`.

**Detail**

- Eight numeric readouts in a 4×2 grid plus a per-core CPU sparkline, refreshed four times a second. The sparkline takes the full row-2 height (32 px) for y-axis resolution.

- **stk** — peak audio-task stack use, as a percentage of its 6 KB allocation. Watermark — never decreases.

- **buf** — minimum pre-buffer fill seen in the last sample window, as a percentage of capacity. 100 % = decoder running well ahead; near 0 % = close to underrun.

- **ram** — internal-heap use as a percentage of total.

- **u** — cumulative count of ring-buffer underruns since boot.

- **to** — seconds remaining before the [Screen Idle](#screen-idle) fade fires. `-` when the backlight-off setting is disabled; `0` once the threshold is past.

- **im** — last IMU motion-delta magnitude in milligrams. Compare against the 50 mg threshold that triggers `markActivity`.

- **c0 / c1** — per-core load as a percentage; the sparkline (cyan core 0, orange core 1) holds ~37 s of history.

**See also**

- [Header](#header) — the strip this row lives in.

# Console Diagnostics

[Up](#diagnostics)

Developer readouts over the USB serial console — no on-screen UI, present in every build.

**Detail**

- **`h` keypress** — prints a one-shot heap census: free bytes and the largest contiguous block, broken down by capability (default / internal / DMA). The largest-block figure is what gates a big allocation like the FLAC decoder's per-frame buffers, so a large free total with a small largest block signals fragmentation.

- **Track-load failure line** — the `loop fail:` log carries `largest=` alongside free heap, so a memory-starved load is distinguishable from other decode failures.

**See also**

- [Audio Formats](#audio-formats) — the FLAC decoder's per-frame allocation is the main consumer these figures track.

# Browser

[Up](#main)
[Down](#fuzzy-search)
[Down](#recent-track-trail)

> [!WARNING]
> Unreviewed.

The main area between the header and footer. Hosts three views: a directory listing (default), a [Fuzzy Search](#fuzzy-search) entered by typing a letter, and a [Visualisation](#visualisation) overlay (waveform and/or spectrum) toggled by `Ctrl+W` / `Ctrl+S` during playback.

The directory listing is a single full-width column of the current directory's entries — directories first (bright cyan), then audio files (light grey), alphabetical within each group. Non-audio files are hidden by default and only appear when the "Hide non-audio" [Settings](#settings) toggle is off; they then render in dim grey at the tail of the list. Long names wrap or clip per `\` toggle. Selected row gets a subtle dark blue-grey tint. The child leading to the folder's most-recently-played track shows in amber (the [Recent-track Trail](#recent-track-trail)). A narrow scrollbar gutter on the right indicates position when the list overflows. Slate-blue frames mark this view. Remains visible during playback.

**See also**

- [Controls](#controls-and-navigation) — wrap mode is toggled by `\`
- [Settings](#settings) — "Hide non-audio" filter controls non-audio visibility

# Recent-track Trail

[Up](#browser)

Each folder remembers the path to the most-recently-played track in its sub-tree. The child on that path — a subfolder, or the track itself — shows in amber, and the cursor rests on it when the folder opens, so following the amber from any level walks straight down to that track; stepping into a sibling instead reveals that branch's own trail. Playing the amber track resumes it where it left off (including mid-track); playing any other row starts from the beginning.

A folder with no tracks of its own still points the way down, since the amber marks the child toward the newest descendant, not a track directly.

**Detail**

- Each folder stores a hidden `.cardplayer` file: the child name, plus the playhead at the leaf. The pointer is written up the whole path when a track starts; the leaf playhead refreshes at boundaries (pause, stop, track change, standby), never during steady playback.

- A deliberate play and auto-play-next both move the trail; alarm fires and track-pick do not. A pointer to a since-removed child yields no marker.

- Storage lives with the card — see [Persisted State](#persisted-state).

# Fuzzy Search

[Up](#browser)

Type-to-search alternative to the directory listing. Pressing any letter opens this view: top row is the typed query, body shows ranked filename matches refreshed each keystroke. `/` plays the selected result and exits to its directory. Backspace-on-empty or `` ` `` (Esc) also exit. Visually distinguished from the directory listing by a green theme.

**Detail**

- Index on SD: `/FZTI.idx` (paths) and `/FZTI.pb` (filters + precomputed lookups). Built once per card, reused as long as the card's reported size hasn't changed — a coarse proxy for card identity that catches most card swaps. Manual rebuild via the Settings "Rebuild index" row.

- Body (~42 KB) is lazy-loaded into RAM only while search is open. Entry pays a ~30 ms SD read; exit frees the memory back to the heap.

- Scoring is filename-only with a strong contiguous-run bonus and no word-boundary bonus. 1-char queries hit a precomputed top-K table; 3+ char queries use the page-bloom filter; 2-char queries do a flat scan.

**See also**

- [Controls](#controls-and-navigation) — `a`–`z` opens; `` ` `` (Esc) exits

# Visualisation

[Up](#main)
[Down](#waveform)
[Down](#spectrum)

Two scrolling overlays drawn over the [Browser](#browser) during playback: a [Waveform](#waveform) (time-domain peaks) on top and a [Spectrum](#spectrum) (frequency-domain spectrogram) below. They can be on independently or together. Each has its own Ctrl shortcut; dismissing always remembers what was on screen, so a later `Tab` flicks the same combination back without re-selecting.

**Detail**

- Dual-mode height fractions: waveform 2/5 on top, spectrum 3/5 below. Single-view mode gives the whole inner area to whichever is on.
- Every dismiss path — `Tab`, `` ` `` (Esc), `del`, or any other key — snapshots the current `(waveform_on, spectrum_on)` pair before clearing. `Tab` outside the overlay restores from the snapshot; cold start with no snapshot shows both views.
- The [Settings](#settings) "Auto waveform" and "Auto spectrum" toggles independently open their view at each new track's start.
- While an overlay is up, transport, volume, brightness, pause and the diagnostics-row toggle pass through; everything else dismisses.

**See also**

- [Controls](#controls-and-navigation) — `Ctrl+W`, `Ctrl+S`, `Tab`, `` ` `` bindings

# Waveform

[Up](#visualisation)

Scrolling time-domain peak overlay — each column is the max absolute sample seen since the previous column boundary, so transients and silence read at a glance.

**Detail**

- Column ring sized for ~4 s of on-screen history; advanced by the audio task on column boundaries.
- Single-view: occupies the full inner area. Dual-view: top 2/5.

# Spectrum

[Up](#visualisation)

Scrolling FFT spectrogram — each new column encodes the magnitude of frequency bins for the current audio slice, with brighter pixels marking louder bins. Low frequencies at the bottom, high at the top.

**Detail**

- Computed on column boundaries from the same audio slices the waveform uses; bin intensities map to a heat-style colour ramp.
- Column ring matches the waveform's ~4 s on-screen history so the two views scroll in lockstep.
- Single-view: occupies the full inner area. Dual-view: bottom 3/5.

# Footer

[Up](#main)

A thin strip along the bottom, always visible — independent of what the browser is showing and of any diagnostics-row toggle. Left to right: a transport icon, the playing track's name (marquee-scrolling when it overflows its slot), and on the right a volume bar with its numeric level. Track position is shown not by a bar but by a thin red line riding left→right behind the name.

The transport icon carries two things at once: playback state by *shape* — triangle playing, two bars paused, square stopped — and loudness-leveling on/off by *colour*, green off, amber on. When nothing is playing the name reads "stopped" and the icon is the square.

The volume bar is a slim vertical column filling bottom→top, sized relative to the [Settings](#settings) "Volume max" cap — a full bar reads as "at your chosen ceiling" rather than the hardware ceiling.

**Detail**

Palette: track name and [Battery](#battery) voltage blue, volume bar and number cyan, progress line red, transport icon green/amber. The progress line and volume bar each stay within the name's text region — never overlapping each other or the icon. A hairline frames the top of the strip so the footer reads as a distinct region.

# Controls and Navigation

[Up](#application)

Most keys mean something specific to the **current screen**; a few are **global** and do the same thing everywhere.

- **Global (every screen)** — `` ` `` (Esc) backs out one level — or, at the [Main](#main) root, opens the [Standby Clock](#standby-clock) — and the transport/display keys (pause, volume, skip, seek, brightness) stay live so playback is always controllable. They yield only while typing in search or at a confirmation modal.
- **Per screen** — the arrow cluster `;` `.` `,` `/` (the keyboard's up / down / left / right) navigates and activates: up/down move the cursor, left steps out, right enters or adjusts — the exact effect depends on the screen. `Enter` and `Del` are likewise screen-local; letters and digits type into [Fuzzy Search](#fuzzy-search) or chess.
- **From [Main](#main)** — `Ctrl`-combos open the other screens: `Ctrl+/` [Settings](#settings), `Ctrl+W`/`S` [Visualisation](#visualisation), `Ctrl+H` [Chess](#chess), `Ctrl+D` [On-screen Diagnostics](#on-screen-diagnostics).

Esc backs out along the edges of the screen tree, toward Main:

```
Main  (browser / search / visualisation)
├ Settings
│ ├ Key Reference
│ ├ Reset Confirmation
│ └ Alarms
│   ├ Set Current Time
│   └ Alarm Editor
│     ├ Days
│     └ Track Picker
├ Chess
└ Standby Clock
   └ Alarm        (interrupt — pre-empts any screen; dismiss restores it)
```

The exhaustive key-by-key list is the on-device [Key Reference](#key-reference); it isn't duplicated here.

**Detail**

- Holding `Ctrl` makes the hardware emit the *shifted* character, so `Ctrl+w` arrives as `W` and `Ctrl+/` as `?`; bindings match those forms.
- Brightness (`Ctrl+-`/`=`) is context-sensitive — the [Standby Clock](#standby-clock) dim level while the clock is up, the normal level otherwise.
- Skip is `Ctrl+[`/`]`; plain `[`/`]` held is seek (a silent scrub while paused).
- Auto-repeat (`;` `.` `,` `/`, volume, seek) is poll-driven while the key is held.

**See also**

- [Key Reference](#key-reference) — the on-device, authoritative binding list
- [Persisted State](#persisted-state) — settings and playhead storage

# Playback

[Up](#application)
[Down](#audio-formats)

Decoding of the current track runs on its own FreeRTOS task, separate from the main UI loop. The task must not risk blocking the ESP32 task watchdog — large ID3v2 tags, for example, can pin the decoder in a multi-second scan for the first audio frame. Decoded samples flow through a pre-buffer, then a small staging ring, into the I2S driver.

**Detail**

- Audio task has 8 KB stack, priority above the main loop and below driver tasks.

- Audio runs on core 0; the main loop runs on core 1. The audio task is explicitly pinned via `xTaskCreatePinnedToCore`. The task watchdog watches core 0's idle task — meaningful protection against a hung decoder.

- Pre-buffer (~150 ms of mono samples) sits between the decoder and the speaker, decoupling decoder-fill from speaker-submit. The decoder runs ahead of audible playback by ~220 ms total (pre-buffer + 3-slot output ring + 2-deep speaker queue), giving the [Visualisation](#visualisation) meaningful future-audio look-ahead.

- User-initiated stop / skip calls `hardFlush` to discard the pre-buffer and the speaker queue — response is instant. Natural track end drains normally so the tail isn't cut, at the cost of a ~150 ms longer inter-track gap.

- At natural track end, advances to the next audio file in the playing folder (filename order); stops when the folder is exhausted. The advance is gated by the [Settings](#settings) "Auto-play next" toggle (on by default); when off, playback stops at end-of-track.

# Audio Formats

[Up](#playback)

The supported formats: FLAC, MP3, AAC, M4A, WAV.

> [!IMPORTANT]
> M4A is AAC inside an MP4 container, decoded by an in-house demuxer that synthesises an ADTS stream for the existing AAC decoder. DRM-free files only — protected (FairPlay) tracks are rejected at parse.

**Detail**

- WAV / MP3 / FLAC / AAC: handed straight to ESP8266Audio's matching `AudioGenerator`.

- M4A / MP4: wrapped in `AudioFileSourceM4A`, which parses the MP4 box structure, walks the sample tables, and feeds 7-byte-prefixed ADTS frames to `AudioGeneratorAAC`. Sample-size table is read from disk through a 1 KB sliding window; chunk offsets and the synthetic-stream offset table sit in internal RAM. Seek snaps to the nearest chunk boundary.

- Files with multi-track containers, non-`mp4a` codecs, unsupported AAC profiles (only AOT 1–4), out-of-range sample rates (>12) or channel configs, or 64-bit chunk offsets above 4 GB are rejected at parse.

# Chess

[Up](#application)

A side activity. Standard 8×8 board, the user plays White; moves typed in `e2e4` notation; the CPU replies. State persists across reboots so a game can be paused indefinitely and resumed. The audio task is untouched while chess is on screen, so music keeps playing.

Entered with `Ctrl+H`; exits on `` ` `` (Esc) or any unrecognised keypress, returning to whatever was on screen. Global transport keys (pause, volume, skip, seek, brightness) pass through without exiting, so music stays controllable mid-game. The visualisation overlay snapshot-dismisses on entry; `Tab` restores it afterwards.

**Detail**

- Full legal-move generation: castling, en passant, promotion-to-queen, check / checkmate / stalemate.

- CPU plays a random legal move (PoC — placeholder for a real engine).

- Game state — the full position plus the chosen CPU difficulty — stored in NVS under a separate `chess` namespace.

- Game-over display offers `n` to start a fresh game; any other key exits.

**See also**

- [Controls](#controls-and-navigation) — `Ctrl+H` entry

- [Persisted State](#persisted-state) — NVS storage

# Standby Clock

[Up](#application)
[Down](#alarm)

A full-screen bedside clock at a dimmed standby brightness: a large `HH:MM`, the weekday top-left and battery top-right (matched in colour), and a two-line next-alarm hint below. Entered by backing out (`` ` ``) from [Main](#main); any key returns to Main. The [Screen Idle](#screen-idle) fade is suppressed here — the dim standby level is itself the resting state.

A warm, low-blue palette keeps it legible at night without raising the backlight. Armed alarms fire from here, and from any screen, via the [Alarm](#alarm) interrupt.

**Detail**

- Redrawn once a second from the RTC. Standby brightness is its own level (a Settings row), separate from the normal brightness.
- The weekday is computed from the date (Zeller's congruence), not read from the RTC's weekday register.

# Alarm

[Up](#standby-clock)

A system interrupt, not a navigable screen: an armed alarm pre-empts whatever is on screen and takes over with the wake clock at normal brightness — the time turns white as a sounding-alarm cue — playing the slot's track. Three ways out: `` ` `` (Esc) silences the alarm and returns to the pre-alarm track at its position but paused; Enter wakes to the music, dismissing the alarm yet leaving its track playing; any other key snoozes for 8 minutes (a countdown badge shows on the clock). It auto-stops after one cumulative hour, like Esc.

Up to five alarms are configured under Settings → [Alarms](#alarms); each fires on its chosen days at its time, playing its track at its volume (with an optional fade-in ramp), or a built-in beep if the track can't be opened.

**Detail**

- Polled once a second against the current minute and weekday; a per-slot watermark prevents re-firing within the same minute.
- While sounding, the slot's track loops. Snooze pauses it and auto-re-fires after 8 minutes; Enter from snooze wakes by restarting the track from the top.
- The Esc / auto-stop restore also brings back the pre-alarm volume; the Enter wake keeps the alarm's volume.

# Persisted State

[Up](#application)
[Down](#player-state)
[Down](#write-model)

User-facing state survives power cycles, held across three stores: the [Player State](#player-state) in the `player` NVS namespace, the [Chess](#chess) game state in its own NVS namespace, and the browser's per-folder breadcrumbs on the SD card. On boot the saved track loads paused at its position — playback only starts on space.

The breadcrumb trail — the most-recently-played track and its position, remembered per folder — lives on the card as hidden files rather than in NVS, so it travels with the card and survives a reflash. See [Browser](#browser).

# Player State

[Up](#persisted-state)

The inventory that comes back the way it was: volume and volume cap, brightness, font size, the wrap / diagnostics / hide-non-audio / auto-play-next / auto-waveform / auto-spectrum toggles, the idle timeout, the current folder + cursor, and the playing track with its playhead — all in the `player` NVS namespace.

The playhead is saved periodically while a track plays, so a power cut resumes within a few seconds of where it had reached.

**See also**

- [Write Model](#write-model) — when these values are written and how loss is bounded

# Write Model

[Up](#persisted-state)

How player state reaches NVS and how loss is bounded. Mutators set a dirty flag; the loop flushes once the flag has held for 5 seconds, so a burst of keypresses or fast scrolling coalesces into a single write. Persistence runs only during normal operation — never as a shutdown step — so whatever was dirty at cutoff is lost.

**Detail**

- Storage is NVS via Arduino's `Preferences`. SD-independent, robust to power loss between writes.

- Missing saved folder falls back to root; out-of-range cursor clamps to the last entry; missing saved track leaves no playback active. Corrupt or unreadable saved data falls back silently to compiled-in defaults, equivalent to a fresh boot.

- `Del` opens a confirmation modal. Enter wipes the saved record and restores every item to its compiled-in default; any other key dismisses.

# Settings

[Up](#application)
[Down](#key-reference)
[Down](#alarms)
[Down](#reset-confirmation)

> [!WARNING]
> Unreviewed.

Full-screen yellow-themed overlay reached by `Ctrl+/`. Surfaces every user-tunable behaviour as a row with its current value at a glance, plus a handful of action rows. The user model is "one screen lists what's adjustable and what each setting is currently set to" — discoverability and self-documentation, with the global keybinds (where they exist) still working from the browser.

Row kinds, each with a distinct state glyph:

- **Toggle** — `[x]` (on) / `[ ]` (off).

- **Numeric** — current value as text (e.g. `16`, `1m`).

- **Action** — `>` ("press enter to activate / navigate").

Navigation: `;` / `.` move the cursor (auto-repeat). `enter` toggles or activates. `,` / `/` adjust — for numeric rows, step the value (auto-repeat across long ranges like volume max 0..64); for toggle rows, `,` turns off and `/` turns on. Volume (`-` / `=`) and pause (`space`) pass through. Dismiss with `` ` `` (Esc), `Del`, or any letter key.

> [!IMPORTANT]
> Every menu reached from Settings — Alarms, Leveling, Settings data, and their sub-menus — honours the same navigation contract: `;`/`.` move, `,`/`/` adjust or activate (`,` off/down, `/` on/up/activate), `enter` activate, `` ` ``/`Del` back. A new menu that diverges from it is a bug.

**Detail**

Row list (in order):

- *Diagnostics* (toggle) — also `` ` `` outside Settings
- *Wrap names* (toggle) — also `\`
- *Hide non-audio* (toggle, default on) — filters the [Browser](#browser) directory listing
- *Auto-play next* (toggle, default on) — gates the natural-track-end advance in [Playback](#playback)
- *Auto waveform* (toggle) — auto-opens [Waveform](#waveform) at track start
- *Auto spectrum* (toggle) — auto-opens [Spectrum](#spectrum) at track start
- *Font size* (numeric)
- *Volume max* (numeric, 4..64) — caps the live volume; the [Footer](#footer) volume bar rescales to it
- *Brightness* (numeric, 8 log-ish steps over 6..255) — applies live via 200 ms ramp
- *Backlight off* (numeric: 15s / 30s / 1m / 5m / off) — drives [Screen Idle](#screen-idle)
- *Key reference* (action) — opens [Key Reference](#key-reference)
- *Rebuild index* (action)
- *Reset all* (action)

All tunables persist via [Persisted State](#persisted-state). Settings uses a dim-yellow selection highlight (`COL_SETTINGS_SEL_BG`) so the mode is unambiguous against the browser (blue) and search (green).

# Key Reference

[Up](#settings)

> [!WARNING]
> Unreviewed.

Read-only sub-screen of [Settings](#settings) listing every key binding by section (Browse, Playback, Adjust, Misc). The authoritative on-device key list. `;` / `.` scroll; any other key returns to Settings. Reached via the "Key reference" row in Settings; it has no direct shortcut.

# Reset Confirmation

[Up](#settings)

A modal guarding the destructive "Reset all": `/` confirms — wiping saved settings back to defaults and returning to the root — while any other key cancels. Reached from the Settings "Reset all" row.

# Alarms

[Up](#settings)
[Down](#set-current-time)
[Down](#alarm-editor)

The alarm-management screen. Lists the five alarm slots — each labelled by its own config, `HH:MM` then a `MTWTFSS` day mask (a letter for an armed day, `_` for off) — with a "Set current time" row at the top and a clock-brightness row at the bottom. Selecting a slot opens its [Alarm Editor](#alarm-editor); disabled slots show dimmed.

# Set Current Time

[Up](#alarms)

Editor for the real-time clock: hour, minute, year, month, day, then a commit row. Seeds its fields from the live clock so the user nudges deltas rather than typing from scratch; committing writes the RTC, recomputes the weekday (Zeller's), and zeroes the seconds.

# Alarm Editor

[Up](#alarms)
[Down](#days)
[Down](#track-picker)

Per-slot editor: enabled toggle, hour, minute, [Days](#days), volume, fade-in ramp (seconds), track, a preview action, and back. The track row opens the [Track Picker](#track-picker); pressing left on it clears the track back to the built-in beep, and a chosen track's full name wraps across up to three dimmed lines beneath the row. Preview dims to the standby clock, waits five seconds, then fires the alarm exactly as a scheduled one would — an honest end-to-end test of the wake experience.

# Days

[Up](#alarm-editor)

Seven day toggles (Monday–Sunday, Mon-first) plus a back row, choosing which days the alarm fires. A sub-screen of the [Alarm Editor](#alarm-editor) so the editor stays scannable; the chosen set is summarised back on the editor's Days row.

# Track Picker

[Up](#alarm-editor)

The [Browser](#browser) reused in "pick" mode to choose an alarm's track — same navigation, but a yellow theme signals that `/` selects a track here rather than starting playback. Opens on the slot's current track (folder navigated, file highlighted) when one is set. Returns to the [Alarm Editor](#alarm-editor) and restores the main browse position, so the detour doesn't move where the user was.

# Screen Idle

[Up](#application)

> [!WARNING]
> Unreviewed.

Power-saving state machine that fades the panel out after a period of no user activity. Audio playback continues throughout — only the screen sleeps.

Three states: **FULL** (user-set brightness), **FADING** (linear ramp toward zero in progress; panel still shows content and pollers keep redrawing), **OFF** (ramp complete, panel dark, redraws suppressed). At the user-tunable timeout (`Backlight off` row in [Settings](#settings)) the state moves to FADING and a 10 s ramp toward brightness 0 begins; when the ramp lands at 0, FADING → OFF. Any keypress, or IMU motion above the threshold, marks activity: from OFF the panel wakes (1 s ramp back to user brightness, plus a full redraw); from FADING it retargets the ramp upward without redrawing.

**Detail**

- Activity sources: keyboard `isChange && isPressed`, and `pollIMUActivity` when frame-to-frame accelerometer delta exceeds `IMU_MOTION_THRESHOLD = 0.05 g`.

- Timeout values: `15s`, `30s`, `1m` (default), `5m`, `off`. `off` disables the fade entirely — useful when on a charger.

- Brightness levels (8): `6, 7, 8, 16, 32, 64, 128, 192, 255`. Floor of 6 because the panel produces no visible backlight at ≤ 5 on this hardware. Default 255.

- `Ctrl+-` / `Ctrl+=` step brightness globally (context-sensitive — see [Controls and Navigation](#controls-and-navigation)); the brightness ramp on user change is 200 ms (responsive without feeling abrupt).

- The [On-screen Diagnostics](#on-screen-diagnostics) row's `to` readout shows the seconds until the fade fires, for debugging idle behaviour.

**See also**

- [Settings](#settings) — Backlight off and Brightness rows live here
- [On-screen Diagnostics](#on-screen-diagnostics) — `to` / `im` readouts
