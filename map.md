# Application

[Down](#screen-layout)
[Down](#playback)
[Down](#controls)

A minimal media player for MP3 and FLAC files on the M5Stack Cardputer.

```
Application
├ Screen Layout
│ ├ Header
│ │ ├ Version
│ │ ├ Battery
│ │ │ └ Emergency Shutdown
│ │ └ Diagnostics
│ ├ Browser
│ └ Footer
├ Playback
│ └ Audio Formats
└ Controls
```

# Screen Layout

[Up](#application)
[Down](#header)
[Down](#browser)
[Down](#footer)

The display is divided into three fixed regions: a header strip at the top, a footer strip at the bottom, and a main area in between. The main area hosts the browser. The header and footer are always present.

# Header

[Up](#screen-layout)
[Down](#version)
[Down](#battery)
[Down](#diagnostics)

A strip at the top of the display, two rows tall (26 px) when development diagnostics are shown, collapsing to one row (10 px) when hidden. The first row carries the version string and battery readout. The second row carries the diagnostics readouts.

# Version

[Up](#header)

The application's semver version string, rendered at the left end of the header in small text. Always visible.

**Detail**

- Stored as an `APP_VERSION` constant in source; bumped by hand when cutting a build.

- Tracked alongside `CHANGELOG.md` at the project root.

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

Second row of the header, showing live device-resource readouts useful during development. Hideable via the diagnostics toggle.

**Detail**

- Four numeric readouts plus a per-core CPU graph, refreshed four times a second.

- **stk** — peak audio-task stack use, as a percentage of its 8 KB allocation. Watermark — never decreases.

- **buf** — ring-buffer headroom: time spent waiting for the speaker to drain before submitting the next chunk, as a percentage of a 2 ms cap.

- **ram** — internal-heap use as a percentage of total.

- **u** — cumulative count of ring-buffer underruns since boot.

- **cpu 0 / cpu 1** — per-core load as a percentage; the overlaid sparkline (cyan core 0, orange core 1) holds ~17 s of history.

# Browser

[Up](#screen-layout)

Fills the main area in a two-column 80/20 style. The left column shows the current directory; the right column previews the highlighted entry's contents, rendered at roughly half brightness as a hint that it isn't the active column yet. Directories first, then audio files, then non-audio files, alphabetical within each group. Directories are light blue, audio files white, non-audio files dim grey. Long names wrap within the column. Entries are tinted with alternating row backgrounds (green-bar printer paper style — black alternating with a faint green-tinged dark tone) so wrapped names stay visibly grouped. Remains visible during playback.

# Footer

[Up](#screen-layout)

A thin strip at the bottom of the display carrying — left to right — the playing track's name (marquee-scrolling when longer than its slot), a progress bar, and a volume bar. The progress bar doubles as the play/pause indicator: slate-blue while playing, mid-grey while paused or stopped. The hairline framing the top of the strip is the same slate-blue as the progress bar so the footer reads as a distinct region. Always visible, independent of what the browser is showing and of any diagnostics-row toggle. When nothing is playing, the name slot shows "stopped".

# Controls

[Up](#application)

Everything the user does is via the Cardputer's keyboard — there is no touch or rotary input. The current bindings:

- `;` / `.` — move selection up / down

- `,` / `/` — step out to parent / enter highlighted directory (left / right arrow)

- `<` / `?` — skip to previous / next track within the playing directory

- `[` / `]` — held: seek backward / forward within the current track. While paused, the position scrubs silently — resume to hear the new spot.

- `1`–`0` — jump to a position within the current track (`1` = 0%, `2` = 10%, …, `0` = 90%)

- `enter` — play the selected track (or descend into a directory)

- `space` — pause / resume

- `-` / `=` — volume down / up

# Playback

[Up](#application)
[Down](#audio-formats)

Decoding of the current track runs on its own FreeRTOS task, separate from the main UI loop. The task must not risk blocking the ESP32 task watchdog — large ID3v2 tags, for example, can pin the decoder in a multi-second scan for the first audio frame. Decoded samples flow through a small ring buffer into the I2S driver.

**Detail**

- Audio task has 8KB stack, priority above the main loop and below driver tasks.

- Audio runs on core 1 with the main loop on core 0, set via `-DARDUINO_RUNNING_CORE=0` (the Arduino-ESP32 default is core 1). The task watchdog watches core 0's idle task only, so keeping audio off core 0 satisfies the no-blocking rule.

- Ring buffer has three slots of 1536 samples, giving the decoder ~100ms of slack before a stall is heard.

- At natural track end, advances to the next audio file in the playing folder (filename order); stops when the folder is exhausted. `<` / `?` provides manual override at any time.

# Audio Formats

[Up](#playback)

The supported formats: FLAC, MP3, AAC, M4A, WAV.

> [!IMPORTANT]
> M4A is AAC inside an MP4 container, decoded by an in-house demuxer that synthesises an ADTS stream for the existing AAC decoder. DRM-free files only — protected (FairPlay) tracks are rejected at parse.

**Detail**

- WAV / MP3 / FLAC / AAC: handed straight to ESP8266Audio's matching `AudioGenerator`.

- M4A / MP4: wrapped in `AudioFileSourceM4A`, which parses the MP4 box structure, walks the sample tables, and feeds 7-byte-prefixed ADTS frames to `AudioGeneratorAAC`. Sample-size table is read from disk through a 1 KB sliding window; chunk offsets and the synthetic-stream offset table sit in internal RAM. Seek snaps to the nearest chunk boundary.

- Files with multi-track containers, non-`mp4a` codecs, unsupported AAC profiles (only AOT 1–4), out-of-range sample rates (>12) or channel configs, or 64-bit chunk offsets above 4 GB are rejected at parse.
