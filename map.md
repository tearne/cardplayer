# Application

[Down](#screen-layout)
[Down](#playback)
[Down](#chess)
[Down](#controls)
[Down](#persisted-state)

A minimal media player for MP3 and FLAC files on the M5Stack Cardputer.

```
Application
├ Screen Layout
│ ├ Header
│ │ ├ Version
│ │ ├ Path Breadcrumb
│ │ ├ Battery
│ │ │ └ Emergency Shutdown
│ │ └ Diagnostics
│ ├ Browser
│ │ ├ Fuzzy Search
│ │ └ Visualisation
│ │   ├ Waveform
│ │   └ Spectrum
│ ├ Footer
│ ├ Settings (unreviewed)
│ │ └ Key Reference (unreviewed)
│ └ Screen Idle (unreviewed)
├ Playback
│ └ Audio Formats
├ Chess
├ Controls
└ Persisted State
```

# Screen Layout

[Up](#application)
[Down](#header)
[Down](#browser)
[Down](#footer)
[Down](#settings)
[Down](#screen-idle)

The display is divided into three fixed regions: a header strip at the top, a footer strip at the bottom, and a main area in between. The main area hosts the browser. The header and footer are always present. Full-screen overlays — [Settings](#settings), [Key Reference](#key-reference), the reset confirmation modal — replace the normal composition while shown.

# Header

[Up](#screen-layout)
[Down](#version)
[Down](#path-breadcrumb)
[Down](#battery)
[Down](#diagnostics)

A strip at the top of the display, 42 px tall when diagnostics are shown, collapsing to 10 px when hidden. The first row carries the battery readout on the left and either the version string or the path breadcrumb on the right. The second row carries the diagnostics readouts; the CPU sparkline within them spans the full header height in its right-side region, drawing over the path/version slot when sparkline pixels reach that high.

While the fuzzy-search index is rebuilding (background, at boot or manually triggered), a one-character spinner cycles in the top-right cell and the version / breadcrumb slot shrinks by one char to make room.

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

[Up](#header)

> [!WARNING]
> Unreviewed.

Second row of the header, showing live device-resource readouts useful during development. Hidden by default; toggle with `` ` ``.

**Detail**

- Eight numeric readouts in a 4×2 grid plus a per-core CPU sparkline, refreshed four times a second. The sparkline takes the full row-2 height (32 px) for y-axis resolution.

- **stk** — peak audio-task stack use, as a percentage of its 6 KB allocation. Watermark — never decreases.

- **buf** — minimum pre-buffer fill seen in the last sample window, as a percentage of capacity. 100 % = decoder running well ahead; near 0 % = close to underrun.

- **ram** — internal-heap use as a percentage of total.

- **u** — cumulative count of ring-buffer underruns since boot.

- **to** — seconds remaining before the [Screen Idle](#screen-idle) fade fires. `-` when the backlight-off setting is disabled; `0` once the threshold is past.

- **im** — last IMU motion-delta magnitude in milligrams. Compare against the 50 mg threshold that triggers `markActivity`.

- **c0 / c1** — per-core load as a percentage; the sparkline (cyan core 0, orange core 1) holds ~37 s of history.

# Browser

[Up](#screen-layout)
[Down](#fuzzy-search)
[Down](#visualisation)

> [!WARNING]
> Unreviewed.

The main area between the header and footer. Hosts three views: a directory listing (default), a [Fuzzy Search](#fuzzy-search) entered by typing a letter, and a [Visualisation](#visualisation) overlay (waveform and/or spectrum) toggled by `Ctrl+W` / `Ctrl+S` during playback.

The directory listing is a single full-width column of the current directory's entries — directories first (bright cyan), then audio files (light grey), alphabetical within each group. Non-audio files are hidden by default and only appear when the "Hide non-audio" [Settings](#settings) toggle is off; they then render in dim grey at the tail of the list. Long names wrap or clip per `\` toggle. Selected row gets a subtle dark blue-grey tint. A narrow scrollbar gutter on the right indicates position when the list overflows. Slate-blue frames mark this view. Remains visible during playback.

**See also**

- [Controls](#controls) — wrap mode is toggled by `\`
- [Settings](#settings) — "Hide non-audio" filter controls non-audio visibility

# Fuzzy Search

[Up](#browser)

Type-to-search alternative to the directory listing. Pressing any letter opens this view: top row is the typed query, body shows ranked filename matches refreshed each keystroke. Enter plays the selected result and exits to its directory. Backspace-on-empty or `Fn+\`` (esc) also exit. Visually distinguished from the directory listing by a green theme.

**Detail**

- Index on SD: `/FZTI.idx` (paths) and `/FZTI.pb` (filters + precomputed lookups). Built once per card, reused as long as the card's reported size hasn't changed — a coarse proxy for card identity that catches most card swaps. Manual rebuild via `~`.

- Body (~42 KB) is lazy-loaded into RAM only while search is open. Entry pays a ~30 ms SD read; exit frees the memory back to the heap.

- Scoring is filename-only with a strong contiguous-run bonus and no word-boundary bonus. 1-char queries hit a precomputed top-K table; 3+ char queries use the page-bloom filter; 2-char queries do a flat scan.

**See also**

- [Controls](#controls) — `a`–`z` opens, `~` rebuilds, `Fn+\`` exits

# Visualisation

[Up](#browser)
[Down](#waveform)
[Down](#spectrum)

Two scrolling overlays drawn over the [Browser](#browser) during playback: a [Waveform](#waveform) (time-domain peaks) on top and a [Spectrum](#spectrum) (frequency-domain spectrogram) below. They can be on independently or together. Each has its own Ctrl shortcut; dismissing always remembers what was on screen, so a later `Tab` flicks the same combination back without re-selecting.

**Detail**

- Dual-mode height fractions: waveform 2/5 on top, spectrum 3/5 below. Single-view mode gives the whole inner area to whichever is on.
- Every dismiss path — `Tab`, `Fn+\`` (esc), `del`, or any other key — snapshots the current `(waveform_on, spectrum_on)` pair before clearing. `Tab` outside the overlay restores from the snapshot; cold start with no snapshot shows both views.
- The [Settings](#settings) "Auto waveform" and "Auto spectrum" toggles independently open their view at each new track's start.
- While an overlay is up, transport, volume, brightness, pause and the diagnostics-row toggle pass through; everything else dismisses.

**See also**

- [Controls](#controls) — `Ctrl+W`, `Ctrl+S`, `Tab`, `Fn+\`` bindings

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

[Up](#screen-layout)

> [!WARNING]
> Unreviewed.

A thin strip at the bottom of the display carrying — left to right — the playing track's name (marquee-scrolling when longer than its slot), a progress bar, and a volume bar. The volume bar fills relative to the [Settings](#settings) "Volume max" cap, so a full bar reads as "at your chosen ceiling" rather than the hardware ceiling. The progress bar doubles as the play/pause indicator: slate-blue while playing, mid-grey while paused or stopped. The hairline framing the top of the strip is the same slate-blue as the progress bar so the footer reads as a distinct region. Always visible, independent of what the browser is showing and of any diagnostics-row toggle. When nothing is playing, the name slot shows "stopped".

# Controls

[Up](#application)

> [!WARNING]
> Unreviewed.

Everything the user does is via the Cardputer's keyboard — there is no touch or rotary input.

`Fn` selects an alternate binding set; plain bindings fire only when `Fn` is not held, and Fn-modified keys with no binding are no-ops. `shift` is encoded into the printable character by the hardware (`shift+/` → `?`). `Ctrl` is used by the visualisation toggles. `opt`, `alt` are exposed by the library but ignored.

The bindings:

- `;` / `.` — move selection up / down (also moves the result cursor in search; in [Settings](#settings), moves the row cursor)

- `,` / `/` — step out / activate the highlighted entry (descend a directory, start a track, play a search result, or adjust a [Settings](#settings) row). Auto-repeats while held.

- `'` — jump to the currently-playing track

- `{` / `}` — skip to previous / next track within the playing directory

- `space` — pause / resume (passes through [Settings](#settings))

- `[` / `]` — held: seek backward / forward within the current track. While paused, the position scrubs silently — resume to hear the new spot.

- `1`–`0` — jump to a position within the current track (`1` = 0%, `2` = 10%, …, `0` = 90%)

- `-` / `=` — volume down / up (auto-repeats; passes through [Settings](#settings))

- `+` / `_` — font size up / down

- `` ` `` — toggle diagnostics row visibility (in [Settings](#settings), dismisses)

- `\` — toggle filename wrapping in the browser

- `?` — open / close [Settings](#settings)

- `Del` — backspace in search; otherwise confirm-prompt to reset saved settings

- `a`–`z` — open [Fuzzy Search](#fuzzy-search), seeded with the typed letter; in search mode, letters / digits / space append to the query; in [Settings](#settings), dismisses

- `~` — rebuild the fuzzy-search index in the background

- `Fn+\`` (labelled "esc") — exit search; dismiss [Visualisation](#visualisation) overlay; dismiss [Settings](#settings)

- `Fn+=` / `Fn+-` — brightness up / down (shifted variants `Fn++` / `Fn+_` also accepted)

- `Ctrl+W` — toggle [Waveform](#waveform) overlay during playback

- `Ctrl+S` — toggle [Spectrum](#spectrum) overlay during playback

- `Ctrl+H` — enter [Chess](#chess)

- `Tab` — toggle the [Visualisation](#visualisation) overlay during playback; restores the last-shown combination of waveform + spectrum

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

Entered with `Ctrl+H`; exits on `Fn+\`` or any unrecognised keypress, returning to whatever was on screen. Music transport keys are not consumed — they'd exit chess and fire normally. The visualisation overlay snapshot-dismisses on entry; `Tab` restores it afterwards.

**Detail**

- Full legal-move generation: castling, en passant, promotion-to-queen, check / checkmate / stalemate.

- CPU plays a random legal move (PoC — placeholder for a real engine).

- Board state stored in NVS under a separate `chess` namespace.

- Game-over display offers `n` to start a fresh game; any other key exits.

**See also**

- [Controls](#controls) — `Ctrl+H` entry

- [Persisted State](#persisted-state) — NVS storage

# Persisted State

[Up](#application)

> [!WARNING]
> Unreviewed.

User-facing state survives power cycles so volume, volume cap, brightness, current folder + cursor, font size, wrap / diagnostics / hide-non-audio / auto-play-next / auto-waveform / auto-spectrum toggles, idle timeout, and the playing track come back the way they were. On boot the saved track loads paused — playback only starts on space. Intra-track position is not persisted.

Emergency shutdown does not save — persistence runs during normal operation, never as a shutdown step. Whatever was dirty at cutoff is lost.

[Chess](#chess) board state persists under a separate `chess` NVS namespace with its own save path — written on every move rather than via the player-state dirty/flush coalesce, since chess writes are rare.

**Detail**

- Storage is NVS via Arduino's `Preferences`. SD-independent, robust to power loss between writes.

- Mutators set a dirty flag; the loop flushes after the flag has been set for 5 seconds — a burst of keypresses or fast scrolling coalesces into one write.

- Missing saved folder falls back to root; out-of-range cursor clamps to the last entry; missing saved track leaves no playback active. Corrupt or unreadable saved data falls back silently to compiled-in defaults, equivalent to a fresh boot.

- `Del` opens a confirmation modal. Enter wipes the saved record and restores every item to its compiled-in default; any other key dismisses.

# Settings

[Up](#screen-layout)
[Down](#key-reference)

> [!WARNING]
> Unreviewed.

Full-screen yellow-themed overlay reached by `?`. Surfaces every user-tunable behaviour as a row with its current value at a glance, plus a handful of action rows. The user model is "one screen lists what's adjustable and what each setting is currently set to" — discoverability and self-documentation, with the global keybinds (where they exist) still working from the browser.

Row kinds, each with a distinct state glyph:

- **Toggle** — `[x]` (on) / `[ ]` (off).

- **Numeric** — current value as text (e.g. `16`, `1m`).

- **Action** — `>` ("press enter to activate / navigate").

Navigation: `;` / `.` move the cursor (auto-repeat). `enter` toggles or activates. `,` / `/` adjust — for numeric rows, step the value (auto-repeat across long ranges like volume max 0..64); for toggle rows, `,` turns off and `/` turns on. Volume (`-` / `=`) and pause (`space`) pass through. Dismiss with `` ` ``, `?`, `Fn+\``, or any letter key.

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

Read-only sub-screen of [Settings](#settings) listing every key binding by section (Browse, Playback, Adjust, Misc). `;` / `.` scroll; any other key returns to Settings. Reached only via the "Key reference" row inside Settings — there is no direct keybind, because `?` now opens Settings rather than this screen.

# Screen Idle

[Up](#screen-layout)

> [!WARNING]
> Unreviewed.

Power-saving state machine that fades the panel out after a period of no user activity. Audio playback continues throughout — only the screen sleeps.

Three states: **FULL** (user-set brightness), **FADING** (linear ramp toward zero in progress; panel still shows content and pollers keep redrawing), **OFF** (ramp complete, panel dark, redraws suppressed). At the user-tunable timeout (`Backlight off` row in [Settings](#settings)) the state moves to FADING and a 10 s ramp toward brightness 0 begins; when the ramp lands at 0, FADING → OFF. Any keypress, or IMU motion above the threshold, marks activity: from OFF the panel wakes (1 s ramp back to user brightness, plus a full redraw); from FADING it retargets the ramp upward without redrawing.

**Detail**

- Activity sources: keyboard `isChange && isPressed`, and `pollIMUActivity` when frame-to-frame accelerometer delta exceeds `IMU_MOTION_THRESHOLD = 0.05 g`.

- Timeout values: `15s`, `30s`, `1m` (default), `5m`, `off`. `off` disables the fade entirely — useful when on a charger.

- Brightness levels (8): `6, 7, 8, 16, 32, 64, 128, 192, 255`. Floor of 6 because the panel produces no visible backlight at ≤ 5 on this hardware. Default 255.

- `Fn+=` / `Fn+-` step brightness globally; the brightness ramp on user change is 200 ms (responsive without feeling abrupt).

- The [Diagnostics](#diagnostics) row exposes `to` (seconds until fade fires) and `im` (last IMU delta in mg) for debugging idle behaviour.

**See also**

- [Settings](#settings) — Backlight off and Brightness rows live here
- [Diagnostics](#diagnostics) — `to` / `im` readouts
