# UI ergonomics

## Intent

A bundled pass of UI ergonomics improvements. They each touch the display-rendering layer and together shift the visual character toward calmer chrome, clearer readability, and easier reach for in-progress information.

**New keybindings**

- `+` / `_` — font one notch bigger / smaller (both shifted forms; volume keeps the bare `=` / `-`). Allows for multiple notches if more are added later.
- `` ` `` — toggle "minimal chrome" on/off: hides the diagnostics line *and* the transport-info footer, reflowing the browser into the freed space.
- `\` — toggle filename wrapping in the browser on/off.
- `?` — open a help screen listing the keyboard mapping.
- Fn+`,` / Fn+`/` — skip to previous / next track within the playing folder. Replaces the current `<` / `?` bindings; `?` is freed for help.
- A "jump back to currently playing track" key (specific binding TBD) that snaps the browser to the playing folder and selects the playing track.

**Visual cleanups**

- Remove the green printer-bar background tint from filenames in the browser.
- Selected browser item rendered as inverted (black-on-white) rather than enclosed in a box.
- Hairline rule between the files and the directories within a browser column.
- Vertical hairline separating the two browser columns.
- Stack-usage diagnostic shown as a proportion bar instead of a numeric peak.
- Ring-buffer headroom bar made narrower, drawn in grey instead of green.
- Volume rendered as a small bar in the footer (keeping the `vol:` label).

## Approach

### Font size — runtime notch index

A single global `g_font_notch` (int, default 0) controls the user-visible text. `+` increments it, `_` decrements, both clamped to a small set of allowed values — three notches initially: `{0, 1, 2}` corresponding to `setTextSize(1)`, `setTextSize(2)`, `setTextSize(3)`. Size 3 will be chunky (only ~13 chars per browser line) but worth trying; if it proves too coarse it can be swapped for a different font at that notch later. Scope: browser entry text and the footer track-name + volume area. The header (version + battery icon) and diagnostics row stay at size 1 — they're glances, not reads, and the header has no spare vertical room. The browser's `ROW_H` and `charsPerLine()` derive from the current notch so layout reflows on toggle.

### Minimal-chrome toggle — `` ` ``

A single global `g_chrome_minimal` (bool, default false), toggled by `` ` ``. When true:

- The diagnostics row (header row 2) is not drawn.
- The footer (track marker, name, volume, progress bar) is not drawn.
- The browser reflows to fill the freed space, giving usable height `SCREEN_H - 9` (only the version + battery row of the header remains) instead of `SCREEN_H - HEADER_H - FOOTER_H`.

This requires `HEADER_H`, `BROWSER_H`, and `FOOTER_Y` to become runtime values derived from `g_chrome_minimal`. Cursor scroll logic and column-draw bounds read the runtime values directly. A small refactor, but the alternative — leaving 25 px of empty space at the top and bottom — defeats the point of the toggle.

### Help screen — full-screen modal at size 1

`?` opens a help overlay that fills the display, drawn at `setTextSize(1)` to fit the keymap comfortably. Any key dismisses; the screen restores to whatever was there. Audio playback continues underneath (audio runs on a separate task and isn't touched). Content is hard-coded — a future change can promote it to a generated form if the bindings keep shifting.

### Track-skip rebinding

The main loop's keypress switch checks `state.fn` first. `Fn`+`,` calls `skipTrack(-1)`; `Fn`+`/` calls `skipTrack(+1)`. The existing `case '<'` and `case '?'` are removed. Plain `,` and `/` continue to mean ascend/descend; the Fn check is what disambiguates. `?` is now free for help.

### Jump to currently playing — `'`

Bound to the apostrophe key (single quote, immediately right of `;` on the Cardputer — natural reach, not used by anything else). Action: load the playing folder into `g_entries`, set `g_cursor` to the playing track's index, redraw the browser.

### Browser visual cleanup

- Drop the printer-bar zebra alternation. Single black background under all entries.
- Selected entry rendered as inverted: black text on a full-width white background rectangle. Replaces the existing mid-grey outline rule.
- Selection's character colour ladder (BRIGHT/NORMAL/DIM by kind) is unaffected — except BRIGHT is now ignored on selection because the row is inverted. NORMAL and DIM still distinguish active vs preview columns.

### Hairlines

A single-pixel mid-grey rule (`0x4208`, the same grey already used elsewhere) drawn between adjacent entries of different kinds (DIR→AUDIO, AUDIO→OTHER) within each browser column. A vertical hairline at `x = COL_PREV_X` separates the active and preview columns.

### Diagnostics row redesign

- Stack usage shown as a horizontal bar (same width footprint as the existing `stk:NNNN` text), filled proportionally to peak / `AUDIO_TASK_STACK`. No numeric readout; the magnitude isn't actionable mid-session.
- Ring-buffer headroom bar narrowed from 60 px to ~30 px and recoloured to mid-grey instead of green. Frees screen real estate and reduces eye-pull from a non-actionable diagnostic.

### Filename wrapping — toggle

A new global `g_wrap_names` (bool, default true; current behaviour preserved). Toggled by `\`. When false, `entryRows()` returns 1 unconditionally and `drawEntry()` truncates names that don't fit on one line, replacing the trailing characters with a single `…`. Selected entries still get the inverted-row treatment, just one row tall.

### Volume — small bar in the footer

`vol:` label kept; the trailing number replaced by a 20-px-wide horizontal bar filled to `g_volume / MAX_VOL`. Bar drawn flush right against the footer edge where the number used to sit. Same colour conventions as the progress bar (filled white, faint outline).

### Map updates — deferred

Following the process-feedback note from 2026-04-26, no map edits are pre-staged in this Approach. The Controls map node will need updating with the new keybindings (and possibly the visual cleanups discussed in a Browser-area node), but those are independent of the build's code and are best handled in a separate, interactive plan-mode pass once the build is in.

## Plan

- [x] **Layout foundation + minimal-chrome reflow.** Add globals `g_font_notch`, `g_chrome_minimal`, `g_show_help`, `g_wrap_names`. Convert `HEADER_H`, `FOOTER_H`, `BROWSER_Y`, `BROWSER_H`, `FOOTER_Y`, `ROW_H`, `CHAR_W` from `constexpr` to inline helpers that read those globals. Add a `FONT_NOTCHES[]` table giving `setTextSize` per notch. In `drawHeader()` skip the diagnostics row when `g_chrome_minimal`, and in the main draw flow skip the body of `drawFooter()` likewise — so the browser fills the freed space automatically.

- [x] **Keyboard rebinding.** In the main loop's keypress switch, add cases for `+` / `_` (font notch +/−), `` ` `` (toggle minimal chrome), `?` (help), `'` (jump to currently playing), `\` (toggle wrap). Add a `state.fn` check that re-routes `,` / `/` to `skipTrack(-1)` / `skipTrack(+1)`. Remove the old `<` and `?` track-skip cases.

- [x] **Browser visual cleanup.** In `drawColumn()`, drop the zebra alternation between `COL_ZEBRA_A` and `COL_ZEBRA_B` — single black background. In `drawEntry()`, render the selection as an inverted block: white background fill across the row, black foreground text. Replaces the existing grey-outline rectangle.

- [x] **Hairlines.** In `drawColumn()`, draw a 1-px mid-grey horizontal rule between adjacent entries of different kinds (DIR→AUDIO, AUDIO→OTHER). Draw a 1-px vertical rule at `x = COL_PREV_X` running the full browser height to separate the active and preview columns.

- [x] **Filename wrap toggle.** When `g_wrap_names` is false, `entryRows()` returns 1 unconditionally and `drawEntry()` truncates names that don't fit on one line, ending the visible portion in `…`.

- [x] **Diagnostics row redesign.** Replace the `stk:NNNN` text with a horizontal proportion bar (occupying roughly the same width footprint) filled to `g_diag_stack_used / AUDIO_TASK_STACK`. Narrow the ring-buffer headroom bar from 60 px to ~30 px and recolour it from green (`0x07E0`) to a mid-grey.

- [x] **Volume bar in footer.** In `drawFooter()`, replace the trailing `vol:N` text with a `vol:` label followed by a 20-px-wide horizontal bar filled to `g_volume / MAX_VOL`. Bar drawn with the same outlined-fill convention used by the progress bar.

- [x] **Help screen.** Add `drawHelp()` that fills the display with the keymap at `setTextSize(1)`. A new global `g_show_help` (bool, default false) gates entry; the keypress handler dismisses on any key, setting `g_show_help = false` and triggering a full redraw to restore the prior screen. Audio playback continues underneath.

- [x] **Jump to currently playing track.** New action triggered by `'`: scan the playing folder via `scanDir()` into `g_entries`, set `g_cur_path = g_play_dir`, locate the playing track's index in the rescanned entries, set `g_cursor` to that index, call `ensureCursorVisible()`, then redraw.

- [x] **Test on device.** Verify each keybinding reachable; font notches `+`/`_` reflow the browser correctly; `` ` `` reclaims the chrome space; `?` opens help and any-key dismisses; `'` snaps the browser to the playing track; `\` toggles wrap; Fn+`,` / Fn+`/` skip tracks; printer-bar gone; selection rendered inverted; hairlines visible at kind boundaries and between columns; stack-usage bar tracks; ring-buffer bar narrower & grey; volume bar tracks volume.

## Log

- Truncation marker for the wrap-off case is `...` (three ASCII dots), not the single `…` glyph the Approach specified. The default 6×8 font is ASCII-only, so a UTF-8 ellipsis would render as three garbage bytes; using `...` keeps the truncation visible. Worth revisiting if a font with an ellipsis glyph is ever loaded.

- Footer height is now derived from the active font notch (`8 * textSize() + 8`), not a fixed 16. Without this, scaled footer text overflows the original 16 px footer and clips the progress bar. Browser height shrinks at higher notches accordingly.

- `pollDiagnostics`, `pollBattery`, `pollSeekKeys`, and the playback progress redraw now bail out when `g_show_help` is set, so background tickers don't overdraw the help overlay (or, in the seek-keys case, accidentally seek when the dismiss key is `[` or `]`).

- Version bumped to `0.7.1` for this test handover.

- `0.7.2`: hairlines weren't visible on first test — `drawEntry`'s bg fill was overwriting the hairline drawn at the top of the next entry. Reordered to draw the hairline after the entry, on the entry's top pixel (just bg, no glyph rows there). Visible now.

- `0.7.3`: diagnostic bars now labelled (`stk`, `buf`) and matched in width/style — both 30 px wide, both grey outline + grey fill. Stack bar previously used the brighter header colour and a wider 48 px footprint; pulled it down to match the buf bar so neither tugs the eye more than the other. Progress bar in the footer is unchanged (still drawn at the bottom of the footer band when a track is loaded).

- `0.7.4`: footer back to fixed 16 px and text fixed at size 1 — only the browser entries scale with the font notch. Earlier scaled-footer attempt was clipping the progress bar at higher notches. Help screen no longer dismisses on key *release*: the previous handler treated any `isChange() && isPressed()` as a dismiss, which fired when `?` was let go while Shift was still held. Now it requires `state.word` to be non-empty (or Enter) before dismissing.

- `0.7.5`: hairlines weren't reading on-device with the spec'd `0x4208` mid-grey. Bumped both the kind-boundary horizontal hairlines and the column-divider vertical hairline to `COL_HEADER_TXT` (`0x7BEF`, the brighter grey already used for header text) so they actually register as separators. Diagnostic bars stay at `0x4208`. Font notches trimmed from `{1, 2, 3}` to `{1, 2}` per user — size 3 dropped.

- `0.7.6`: hairlines now drawn between every adjacent entry, not just at kind boundaries (Approach said kind-only). User chose every-entry after seeing it on-device — kind-only didn't read as a list separator at the density of the active column.

- `0.7.7`: minimal-chrome toggle (`` ` ``) now hides only the diagnostics row, not the transport footer (Approach and Intent both said both). User chose to keep the lower footer always visible after seeing it on-device — losing track name / progress / volume during browsing was too much. `footerH()` is now a constant 16; `drawFooter()` no longer bails on `g_chrome_minimal`. Diagnostics-row suppression is unchanged.

- `0.7.8`: a third font notch added between the existing two — `fonts::AsciiFont8x16` (fixed monospace 8×16). The Approach allowed for swapping a notch's `setTextSize` value for a different font ("if it proves too coarse it can be swapped for a different font at that notch later"); on-device feedback after `0.7.7` was that `{6×8, 12×16}` left no comfortable middle. Notches are now `{Font0×1, AsciiFont8x16×1, Font0×2}`. `FONT_NOTCHES` is now a struct table carrying font + text-size + char-width + row-height per notch, since per-notch sizing no longer derives from `setTextSize` alone. `drawColumn` resets to `Font0` after the entry loop so the chrome draws (which still use only `setTextSize(1)`) are unaffected.

- `0.7.9`: 1 px gap inserted between each hairline and the top of the glyph row below it (previously the hairline sat directly against the next entry's glyphs). Cursor y-offset bumped from `+1` to `+2` in `drawEntry`; row heights for the two tightest notches grew by 1 to keep the glyph from overlapping the next entry's hairline (notch 0: 9→10, notch 1: 17→18; notch 2 already had headroom).

- `0.7.10`: horizontal hairlines added at the top and bottom rows of the browser area to separate it from the header and footer. File text recoloured from off-white (`COL_FILE_NORMAL=0xBDF7`, `COL_FILE_DIM=0x7BEF`) to slate-blue / dim slate-blue (`0x6979` / `0x41D2`) so files read as a different shade of blue from dirs (which are cyan-blue). `COL_FILE_BRIGHT` left as white — it's used only by the empty-browser placeholder and the help screen, neither of which should turn blue.

- `0.7.11`: bug — entries near the bottom of the browser were spilling into the footer when scrolled. `drawColumn` was checking `y < y_bottom` (entry top within bounds) but not whether the *full entry height* fit, so the last partly-fitting entry painted past the browser. Compounded by `0.7.10`'s bottom separator occupying the last browser row. Fixed: both `drawColumn` and `ensureCursorVisible` now require the entry to fit within `by..by+bh-2` (one row reserved for the bottom separator).

- `0.7.12`: notch 1 font swapped from `AsciiFont8x16` (chunky retro 8×16) to `FreeMonoBold9pt7b` (Adafruit GFX classic terminal mono, 11×18, bold). User found the retro look unappealing and asked for alternatives at the same size class — the FreeMono family is the only other monospace at ~16 px in M5GFX. Bold chosen over regular for legibility at this DPI. Fewer chars per active-column line (~17 vs ~24) as a result of the wider glyph; if filenames truncate too aggressively, can revisit.

- `0.7.13`: middle notch dropped — neither bundled candidate (`AsciiFont8x16`, `FreeMonoBold9pt7b`) read well enough to keep, and pulling in U8g2 / converting a third-party font wasn't worth the time right now. Notches are back to the originals: `{Font0×1, Font0×2}`. Notch 0's row-height stays at 10 (one taller than the pre-`0.7.9` value of 9, to preserve the hairline-to-glyph gap). The `FontNotch` struct stays even though only one font is in use — leaves the door open for a future bring-your-own-font without re-plumbing.

- `0.7.14`: when the entry list overflows the browser, the entry that crosses the bottom edge now renders partially clipped (rather than being skipped, which left a blank gap). Visually conveys "scroll for more". `drawColumn` now sets a clip rect to the column's browser-region bounds and loops while `y < y_max` (any usable row remains); clipping handles the cut-off. `ensureCursorVisible` unchanged — cursor still required to fit fully.

- `0.7.15`: preview column (narrow, right-side) now never wraps filenames, regardless of the global wrap toggle — wrapping there contradicted the "mostly off-screen" visual. `entryRows` and `drawEntry` gained a per-call `wrap` parameter; `drawColumn` derives it as `is_active && g_wrap_names`, so only the active column responds to the toggle. `ensureCursorVisible` (active-column-only) keeps using `g_wrap_names` directly.

- `0.7.16`: single-line entries (preview column always; active column when wrap is off) no longer end in `...` when the name overflows. The column's clip rect cuts the rendering off cleanly at the edge, which reads as "the name continues off-screen" — what we actually want for the preview, and consistent enough for the active column too. Drops the per-character truncation logic; `drawEntry`'s non-wrap branch is now just `setCursor` + `print`.

- `0.7.17`: long single-line names were still rendering wrapped because M5GFX's `print()` does its own auto-wrap at the screen edge — our clip rect masked the visual but the cursor was being pushed to a new line, so a name wider than the column reappeared on the row below. Calling `setTextWrap(false, false)` at the top of `drawColumn` disables that behaviour for entry rendering; names now clip cleanly at the column edge as intended.

- `0.7.18`: default font notch flipped from `0` (small, 6×8) to `1` (large, 12×16). User preference after seeing both on-device.

## Conclusion

Completed. Notable deviations from the Approach:

- **Minimal-chrome scope reduced** (`0.7.7`): the toggle now hides only the diagnostics row, not the transport footer — losing track name / progress / volume during browsing was too much.
- **Font-size mechanism redesigned** (`0.7.8` onward): the planned `setTextSize`-only notch table grew into a struct carrying `font + size + char_w + row_h` per notch after experiments with intermediate fonts. Both candidates (`AsciiFont8x16`, `FreeMonoBold9pt7b`) were ultimately rejected; ended back at the original `{Font0×1, Font0×2}` notch pair, with the larger notch now the default.
- **Bonus visual additions** beyond the Approach: top + bottom browser-area separator hairlines, partial-clip rendering for entries that cross the bottom edge ("more below" hint), preview column always single-line regardless of wrap toggle, plain edge-clipping in place of `...` truncation, file colour shifted from off-white to slate-blue to read as a different blue from dirs.

**Documentation impact**: the Controls and Browser map nodes will need updates in a separate plan-mode pass — new keybindings (`+ _ \` \\ ' ?`, Fn+`,` Fn+`/`), the wrap toggle, the new visual frame, and the dir/file colour distinction. Per the Approach, those map edits were deferred and not pre-staged here.
