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
│ │ └ Diagnostics
│ ├ Browser
│ └ Footer
├ Playback
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

A two-row strip at the top of the display (~18px total). The first row carries status indicators and transient notification banners. The second row carries development-time diagnostics.

# Version

[Up](#header)

The application's semver version string, rendered at the left end of the header in small text. Always visible.

**Detail**

- Stored as an `APP_VERSION` constant in source; bumped by hand when cutting a build.

- Tracked alongside `CHANGELOG.md` at the project root.

# Battery

[Up](#header)

A small icon at the right end of the header showing remaining charge. Icon only — no percentage text.

**Detail**

- Icon 27px wide × 8px tall, at the right end of the header.

- Read via `M5.Power.getBatteryLevel()` (returns 0–100, negative if unavailable).

- Polled every few seconds; the reading changes slowly.

- Fill colour by level: `> 80%` green, `> 40%` blue, `> 20%` yellow, `> 10%` red, `≤ 10%` bright red.

# Diagnostics

[Up](#header)

Second row of the header, showing live readouts useful during development. Present regardless of what the user is doing.

**Detail**

- **stk** — peak stack usage of the audio task (numeric, e.g. `stk: 3100`).

- **buf** — small horizontal bar. Width reflects the margin between decoder and speaker (time spent waiting for the speaker to drain before submitting the next buffer). Near-full means the decoder is ahead; near-empty means it's only just keeping up.

- **u:** — cumulative count of ring-empty events. Zero means no audible glitches.

# Browser

[Up](#screen-layout)

Fills the main area in a two-column 80/20 style. The left column shows the current directory; the right column previews the highlighted entry's contents, rendered at roughly half brightness as a hint that it isn't the active column yet. Directories first, then audio files, then non-audio files, alphabetical within each group. Directories are light blue, audio files white, non-audio files dim grey. Long names wrap within the column. Entries are tinted with alternating row backgrounds (green-bar printer paper style — black alternating with a faint green-tinged dark tone) so wrapped names stay visibly grouped. Remains visible during playback.

# Footer

[Up](#screen-layout)

A thin strip at the bottom of the display carrying track name, play/pause state, a progress bar, and the volume level. Always visible, independent of what the browser is showing. When nothing is playing, shows "stopped".

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

Decoding of the current track runs on its own FreeRTOS task, separate from the main UI loop. The task must not risk blocking the ESP32 task watchdog — large ID3v2 tags, for example, can pin the decoder in a multi-second scan for the first audio frame. Decoded samples flow through a small ring buffer into the I2S driver.

**Detail**

- Audio task has 8KB stack, priority above the main loop and below driver tasks.

- Audio runs on core 1 with the main loop on core 0, set via `-DARDUINO_RUNNING_CORE=0` (the Arduino-ESP32 default is core 1). The task watchdog watches core 0's idle task only, so keeping audio off core 0 satisfies the no-blocking rule.

- Ring buffer has three slots of 1536 samples, giving the decoder ~100ms of slack before a stall is heard.

- At natural track end, advances to the next audio file in the playing folder (filename order); stops when the folder is exhausted. `<` / `?` provides manual override at any time.
