# Settings screen

## Intent

The user-tunable behaviours — name wrapping, diagnostics visibility, font size, and others as they appear — are currently invisible until you've read the help screen, and even then only the keybind is shown, not the current state. A new screen lets the user discover what's adjustable, see each setting's current value, learn the key that toggles it, and change it from within the screen itself. Discoverability and self-documentation, with the keybind shortcuts continuing to work as they do now.

This change is closely related to `persisted-state`: the same set of toggles is the natural target for both surfacing (here) and persistence (there). Build order is worth thinking through — whichever lands first informs the other's surface area.

## Approach

### Contents

The screen carries every user-tunable behaviour plus a couple of related actions, grouped:

- **Toggles** — diagnostics row, filename wrap, hide non-audio files (new, default on), auto-show waveform on play (new), auto-play next track (new — currently always on, becomes a toggle).
- **Numeric** — font size, volume max (new), screen idle timeout (new).
- **Actions** — rebuild fuzzy index, reset saved settings.


### Entry: repurpose `?`

`?` opens the settings screen; the current help overlay is removed. Each tunable row carries its keybind label, so the screen surfaces every configurable behaviour and the key that controls it. A **"Key reference"** row, when activated, opens a separate read-only screen listing the full keymap (navigation, transport, search) so that material isn't lost when the help overlay goes.

### Layout

A single column of rows. Each row: name on the left, state glyph in the middle, keybind hint on the right. The state glyph distinguishes the three row kinds at a glance:

- **Toggle** — `■` (on) / `□` (off).
- **Numeric** — the current value as text (e.g. `14`, `10`).
- **Action** — `›`, signalling "press enter to activate/navigate".

`;` / `.` move selection. `enter` toggles a boolean, activates an action row, or opens the key-reference sub-screen. Numeric rows are adjusted with `,` / `/` while selected — chosen to avoid colliding with the live global keys (`+`/`_`, `-`/`=`), which keep working unchanged. `Fn+\`` (esc) or `?` dismisses. The reset action keeps its existing confirm modal.

### New tunables

- **Hide non-audio files** — when on (default), non-audio entries are filtered out of the browser listing entirely (not just dimmed). Fuzzy search already filters to audio files (`isAudioExt` at fuzzy_index.cpp:238), so this setting affects the directory listing only.
- **Volume max** — caps the upper bound of the live volume control. Range 0–10 (matching `MAX_VOL`), default 10 (no cap). When the cap is lowered below the live value, live volume clamps down to match.
- **Auto-show waveform on play** — when on, the waveform overlay opens automatically at the start of each track. Dismissal during a track works exactly as today (any other key dismisses); the auto-show fires again at the next track's start.
- **Auto-play next track** — when on (default), natural track end advances to the next file in the folder, as today. When off, playback stops at end-of-track.
- **Screen idle timeout** — controls the existing screen-blank-idle state machine's time-to-dim threshold. The dim-to-off offset (currently 60 s) stays fixed. Discrete steps: `off`, `1m` (default, today's behaviour), `5m`. `off` disables blanking entirely (useful on charger).

### Existing keybinds

All current per-setting binds (`` ` ``, `\`, `Del`, `Ctrl+W`, `~`, `+/_`, `-/=`) keep working outside the screen, per Intent. The screen is purely additive for those.

### Persistence

New tunables (hide-non-audio, volume max, auto-waveform, auto-play next, screen idle timeout) join the existing NVS-persisted set via the same dirty-flag/5-second-flush mechanism. No new storage layer.

### Map impact

Catch-up after build, negotiated per node:

- **New node: Settings** under Screen Layout, describing the overlay and the rows it carries.
- **Browser** — note the hide-non-audio filter and its default.
- **Controls** — `?` now opens settings; help overlay removed.
- **Waveform Visualisation** — no longer "not exposed"; references the auto-show setting.
- **Persisted State** — new persisted keys appear in its list.
- **Screen Idle (or wherever screen-blank-idle was mapped)** — note the time-to-dim threshold is now user-tunable; off is reachable.

## Plan

- [x] Settings view scaffolding: state flag, `?` opens, `Fn+\`` / `?` dismisses, header + footer remain
- [x] Row data model: ordered list with kind (toggle / numeric / action), name, state accessor, optional keybind label
- [x] Render rows: name, glyph (`■`/`□` / value / `›`), keybind column, selection highlight
- [x] Row navigation with `;` / `.`
- [x] Toggle activation with `enter`
- [x] Numeric adjust with `,` / `/`
- [x] Action activation with `enter`, preserving reset's confirm modal
- [x] Key-reference sub-screen: read-only listing of all bindings, dismissed back to settings
- [x] Wire diagnostics, wrap, font size, rebuild-index, reset-settings rows to existing state
- [x] Hide non-audio files: directory listing filter, default on, NVS-persisted
- [x] Volume max: clamp `g_volume` on lower, NVS-persisted, 0–10 range, default 10
- [x] Auto-show waveform on play: trigger overlay at track start, NVS-persisted
- [x] Auto-play next track: gate end-of-track advance, NVS-persisted, default on
- [x] Screen idle timeout: plumb `off`/`1m`/`5m` to screen-blank state machine, NVS-persisted, default `1m`
- [x] Remove the existing help overlay and its render path
- [x] Bump APP_VERSION and add CHANGELOG entry

## Conclusion

Shipped as 0.18.19. Final scope grew substantially beyond the Plan via iteration: brightness as a tunable (with Fn shortcut), three-state idle machine (FULL/FADING/OFF) so the display keeps updating until it actually goes dark, volume scale bumped 10 → 64, footer volume bar rescaled to the user's chosen cap, key repeat on volume and adjust keys, mode-coloured selection palette, and two new diagnostics readouts (`to`, `im`) that earned their place during the timeout-debugging detour.

Map catch-up is deferred — see the Map section, where the new and changed nodes were bulk-drafted with `[!WARNING] Unreviewed` callouts. Each is to be negotiated per-node when the user revisits.

Changelog entry: rolled into a single 0.18.19 line; the iterative 0.18.0–0.18.19 chain is preserved here in the Log.

## Log

- Settings screen is full-screen (matching the existing help overlay's pattern) rather than overlaying with header/footer preserved. The Approach didn't pin this down once we picked "repurpose `?`"; matching the help convention was simpler and consistent.
- State glyphs use `[x]` / `[ ]` and `>` rather than `■` / `□` / `›`. LovyanGFX's Font0 is ASCII-only — the original Unicode glyphs from the Approach would render as boxes. ASCII fallbacks still read clearly at small sizes.
- "Remove existing help overlay" was reinterpreted as repurposing: the help renderer (`drawHelp` / `HELP_ENTRIES`) is reused as the Key Reference sub-screen reached from Settings, rather than deleted. Saves ~90 lines of duplicated rendering. The `?` keybind now opens Settings; the help entry list was updated to reflect the new world (the `?` row reads "Settings" and the `Del` row is removed since Reset is reached from Settings rather than being its own help-screen line).
- Added a small `overlayActive()` helper to consolidate the half-dozen `g_show_help || g_show_reset_modal` guards now that there's a third overlay (settings) to factor in.
- Build: 0.18.0 firmware compiles successfully on both environments. Flash usage 27 % (was 26 % at 0.17.44), DIRAM 38.9 %.
- 0.18.1: backtick (`` ` ``) dismisses settings — saves a `Fn+\`` reach for a key with no role inside settings. State glyph/value right-justified with proper width-based positioning; the keybind column was removed (the Key Reference sub-screen covers that material already), which also fixes overlap with longer row names.
- 0.18.19: Boot-time brightness apply. Persisted `bright` value is loaded into `g_brightness_idx` by `loadState()`, but the panel had already been set to the compiled-in default earlier in `setup()` — the saved value only took effect on the first keypress (which retargeted the ramp via `markActivity`). Fix: re-apply `userBrightness()` to the panel and re-seed the ramp state immediately after `loadState`. Now the saved brightness is honoured from the moment the screen comes up.
- 0.18.18: Intermediate `SCREEN_FADING` state inserted between `FULL` and `OFF`. The poll fires at the timeout, kicks off the brightness ramp, and moves to `FADING`; `OFF` is only entered once `pollBrightnessRamp` sees the ramp complete at brightness 0. Display redraws (marquee, waveform, progress, diagnostics) keep running through the fade — the user can still watch the progress bar advance, the marquee scroll, and the `to`/`im` cells update right until the panel actually goes dark. Wake from `FADING` retargets brightness without forcing a full redraw (the canvas was up to date already).
- 0.18.17: Diagnostics text colour bumped from 0x7BEF (mid-grey) to a new COL_DIAG_TXT = 0xC618 (light grey, matches COL_FILE_NORMAL). The other COL_HEADER_TXT uses — battery icon, hairlines, version, breadcrumb — keep the dimmer value so the visual hierarchy is preserved.
- 0.18.16: Diagnostics row swaps the `L` (largest-free) and `M` (peak-pressure) heap readouts for `to` (countdown to backlight-off fade, in seconds) and `im` (last IMU motion-delta magnitude in mg). With the user reporting that the 15s timeout "doesn't seem to be working", visible idle + motion readouts give a non-serial way to see what the state machine is doing — `to` should count down and `im` should sit near 0 on a still desk; a non-decreasing `to` or a high `im` would point to the cause.
- 0.18.15: kept brightness levels 6, 7, 8 (user-confirmed all three meaningful).
- 0.18.14: Brightness floor raised from 1 to 5 — on this hardware values below ~5 produce no visible backlight, so 1 and 4 were indistinguishable from the off state and a hazard inside Settings (easy to land on, hard to recover from without seeing). Levels now `5, 8, 16, 32, 64, 128, 192, 255` (8 steps).
- 0.18.13: Brightness levels switched to roughly-logarithmic spacing (`1, 4, 8, 16, 32, 64, 128, 192, 255`) — the eye perceives dim changes more strongly than bright ones, so log spacing makes each step feel evenly different. Floor is `1`, the dimmest non-zero value the API accepts (0 would be indistinguishable from the off-timeout state). Settings now displays the raw hardware value (1..255) rather than a synthetic 1..N step number, so the on-screen value matches what `setBrightness()` actually receives.
- 0.18.12: Screen-idle state machine simplified — the SCREEN_DIM intermediate is gone. At timeout, the screen now starts a single 10-second linear fade to 0 (the previous "dim then off" pair was harder to predict and explain). The idle row in Settings is renamed "Backlight off". New "Brightness" setting with 8 discrete levels (32/64/96/128/160/192/224/255) — the LovyanGFX `setBrightness` API accepts 0..255, and 8 stepped values give an obviously-different press without the granularity ever feeling fiddly. Default = level 8 (255). Brightness ramps over 200 ms on user change so each press feels responsive but not jarring. Global Fn+= / Fn+_ / Fn+- / Fn++ as brightness up/down shortcuts (shifted and unshifted accepted on both sides); works from browser and from inside Settings. Wake from off now ramps to `userBrightness()` rather than a hardcoded `BRIGHT_FULL`.
- 0.18.11: Idle-timeout steps gain `15s` and `30s` at the head of the list (`15s, 30s, 1m, 5m, off`). Default `1m` shifts from index 0 to 2; saved indices from earlier 0.18.x builds will land on different values (old idx 0 `1m` → now `15s`; old idx 1 `5m` → now `30s`; old idx 2 `off` → now `1m`) — one-time correction in Settings. Volume (`-`/`=`) and pause (`space`) now pass through while Settings is open, with the footer redraw suppressed under the overlay; volume keys auto-repeat regardless of whether Settings or the browser is on screen.
- 0.18.10: Volume max floor of 4 (`VOLUME_MAX_MIN`) so the cap can't trap users at a muted device from inside Settings. Auto-repeat extended to the global `-` / `=` volume keys (same 400 / 100 ms cadence) — held `-` ramps volume down smoothly across the new finer 0..64 scale.
- 0.18.9: Browser selection blue toned down a notch (0x6979 → 0x4978) — the frame value stayed bright, but the filled selection row was competing with it. Now sits as filled-but-recessive. Both still survive RGB332 quantisation.
- 0.18.8: `g_volume_max` default 64 → 16. A fresh boot now has the safer cap so the footer bar (which scales to the cap) reads as "full at your chosen ceiling" rather than ~25 % of hardware max. Users raise the cap deliberately when they want headroom.
- 0.18.7: Footer volume bar scales to `g_volume_max` rather than absolute `MAX_VOL`. A "full" bar now reads as "at your chosen ceiling" rather than "at the hardware ceiling" — useful when you've set a lower cap.
- 0.18.6: Auto-repeat on `;` / `.` / `,` / `/` while Settings is shown (400 ms initial delay, 100 ms repeat — matching the existing browser-nav cadence). Lets the user race through long numeric ranges like volume max (0..64) without 64 individual presses. Default boot volume bumped 32 → 16; mid-scale was too loud for a soft-start.
- 0.18.5: `,` / `/` (left / right) now also act on toggle rows — `,` turns off, `/` turns on. One key pair adjusts every row kind, no need to remember when to press enter vs left/right.
- 0.18.4: Settings screen gets its own scrollbar, mirroring the browser/key-reference pattern. Thumb sized to viewport coverage and tinted with the settings yellow so it reads as a mode-coloured frame element. Value column now leaves a 5 px gutter on the right when the scrollbar is drawn so the thumb doesn't overlap the right-justified state text.
- 0.18.3: Mode-coloured selection highlights. Browser's selection adopts the brighter slate-blue (`COL_SELECTION_BG` value bumped 0x2190 → 0x6979, matching the frame), making the selection more visible. Settings gets its own dim-yellow highlight (`COL_SETTINGS_SEL_BG = 0x6320`) so the three modes — browse (blue), search (green), settings (yellow) — are unambiguous at a glance. Both new values survive the 8 bpp RGB332 quantisation.
- 0.18.2: `MAX_VOL` bumped 10 → 64 for finer volume granularity (the 0–10 user-facing scale was a coarse-UX choice, not a hardware limit — underlying `setVolume(0..255)`). Defaults adjusted (g_volume 5 → 32, mid-scale). Existing NVS-saved `vol` values from earlier builds will load as much quieter — equivalent to old-units × ~6.4 on the new scale; user should expect a one-time volume-down on first boot from a pre-0.18.2 device. Idle-timeout label order changed to `1m`, `5m`, `off`. Saved `idleto` indices from 0.18.0/0.18.1 will misalign by one (old 1=`1m` → now 1=`5m`; old 0=`off` → now 0=`1m`); also a one-time minor surprise. Settings selection highlight now uses `COL_SELECTION_BG` (matching the browser) rather than `COL_BROWSE_FRAME`. Letter keys dismiss settings, matching the waveform overlay's pattern — useful for users reaching for fuzzy search.

