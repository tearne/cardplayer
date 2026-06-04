# Footer progress marker

**Mode:** Wander

## Intent

Rework the footer to give the playing track's name as much room as possible, and show progress more minimally:

- Move the volume number to the **right** of the volume bar (currently on its left).
- **Remove the progress bar** entirely, handing its space to the track-name slot.
- Indicate track progress another way — a small marker (leaning toward a downward-pointing equilateral triangle, its top edge aligned to the footer separator line, apex pointing down) riding the **full screen width** left→right with playback position. Spanning the whole width makes it *more* precise than the old bar, which was confined to its ~96 px slot; the apex gives a 1 px position reference.

Rough idea, open to refinement — the exact marker shape and how the freed space is shared are to be settled as the work proceeds.

## Log

- Footer reworked: height 10 → 12 px to carve a 3 px **marker lane** at the top (over the separator) with the name/volume text row below it — so the marker never collides with text and partial redraws stay cheap. Browser loses 2 px.
- Progress bar removed; name slot widened 106 → 204 px (runs to just before the volume slot). Volume reordered: bar on the left, two-char level to its right.
- `drawProgressMarker`: a downward triangle (half-width = lane-height−1 → ~5 px wide × 3 px tall), apex pointing down, positioned across the full width by `pos/size`. Green playing / blue paused / amber while leveling (same state palette as the old bar). The three progress-update sites (jump-to-tenth, held-seek, the 500 ms loop refresh) now repaint just the marker lane.
- Built clean at 0.26.6; awaiting hardware look.
- Reworked per feedback (0.26.7): reverted the footer to 10 px (no dedicated lane) and made the triangle large — height 8 px, width 11 px (`FOOTER_MARKER_H` 8, half-width 5). The triangle is drawn *behind* the name/volume, which now render with a **transparent** background so they overlay it where they coincide (the triangle shows through around the glyphs). Footer is now always recomposed as a unit via `composeFooter()` (clear → marker → name → volume); every update path (marquee, jump, held-seek, 500 ms refresh) routes through it so the marker-under-text layering stays consistent. `setFooterText` switched to transparent bg.
- Marker colours dimmed (0.26.8): the bright mint green (lum ~204) clashed with the white text over it. Switched the marker to dim state colours — green `0x43E7`, blue `0x41CF`, amber `0x7A80` (~half luminance) — so white text reads over them while the triangle still shows on black. (These are marker-only; the brighter blue/green stay on the battery/volume and elsewhere.)
- Switched footer text to the blue accent (0.26.9): the white-text-vs-marker fight was on brightness. With the name now blue (matching volume + battery), the marker is separated by hue instead, so it can be bright again — yellow-green playing `0x7FE0`, grey paused `0x7BEF`, amber leveling `0xFD20`. The marker must avoid blue (paused is grey, not blue) so it doesn't disappear under the blue text.
- Tuning (0.26.10): playing marker dimmed from the glaring `0x7FE0` to a medium yellow-green `0x5520` (hue carries the distinction, not brightness); volume moved to white `0xFFFF` to stand apart from the now-blue track name. Footer palette: name blue, volume white, marker green/grey/amber, battery voltage blue.
- Marker changed from triangle to a disc (0.26.11): filled circle radius 4 (`FOOTER_MARKER_R`) centred on the separator line (`footerY()-1`), drawn *on top* of the text it crosses (occludes it) rather than behind. The footer now composes an extended region (`footerDrawY/H` = band + 4 px above) so the disc's upper half is cleared/presented each update; composeFooter repaints the separator hairline (`browseFrameColor()`) since the clear erases it. Same green/grey/amber state colours.
- Tuning (0.26.12): disc enlarged (radius 4→5), recoloured purple `0xA81F` (playing) and moved *behind* the text and separator (composeFooter order: disc → separator → name → volume); `footerDrawY/H` corrected to cover the disc's full top (centre at footerY()-1, so top is R+1 above). Volume switched to cyan `0x07FF` to stand apart from the blue name. Footer palette now: name blue, volume cyan, marker purple/grey/amber, battery blue.
- Tried a different marker (0.26.13): replaced the disc with a 1 px vertical line at the play position, spanning the separator down to the screen bottom, rendered behind everything (composeFooter: line → separator → text). Removed `FOOTER_MARKER_R`; `footerDrawY/H` shrank back to band + the 1 px separator row. Keeps the purple/grey/amber state colours and cyan volume.
- Marker line set to plain white `0xFFFF` (0.26.14), dropping the per-state (purple/grey/amber) colours — the `COL_MARKER_*` set collapsed to a single `COL_MARKER`. The line no longer signals play/pause/leveling by colour.
- Reverted to a state-coloured line (0.26.15) and fixed the weak playing contrast: playing is now bright pink-magenta `0xFA1F` (near-complementary to the blue text; the earlier purple sat too close to blue), grey paused, amber `0xFD20` leveling (kept — liked).
- Reworked leveling indication (0.26.16): the progress line is now always amber (hue was indistinguishable at 1 px, so it no longer carries play-state). Leveling-on is shown instead by a small "compress" icon at the far left — two amber triangles pointing inward (▷◁) — with the track name indented past it (`FOOTER_ICON_SLOT`); the marquee extent accounts for the indent. Icon drawn in composeFooter after the separator.
- Leveling icon rotated 90° to a vertical squeeze (▽ over △) and recoloured cyan (`COL_FOOTER_VOL`) — 0.26.17.
- Leveling icon made outline-only (0.26.18, `drawTriangle`) — the filled cyan was too bright/heavy.
- New leveling symbol (0.26.19): replaced the triangles with an 8×8 outline square containing a compressor-knee curve (rise ~1:1 then flatten), cyan, no fill. Icon width 7→8.
- Confined the progress line to the name/text area (0.26.20): maps across `FOOTER_NAME_X .. +FOOTER_NAME_W` (was full screen width), so it starts at the text start and ends at the text-area end rather than running under the volume. With the line clear of the volume, volume + leveling icon recoloured to amber (`COL_FOOTER_VOL` 0xFD20; icon now uses `COL_MARKER`). Footer: name blue, volume/line/icon amber, battery blue.
- Line start made icon-aware (0.26.21): the span now begins past the leveling icon when shown (`FOOTER_NAME_X + indent`), so the start sits at the text start rather than on the icon.

- Vertical volume bar + cyan (0.26.22): volume recoloured back to cyan (`COL_FOOTER_VOL` 0x07FF) to drop the amber "warning" read. The volume bar turned **vertical** — a 5 px-wide outline column the full footer height, filling bottom→top with level, number unchanged to its right. The slim column shrank the volume slot 32 → 20 px, so the name slot widened 204 → 216 px. Footer: name blue, volume cyan (vertical bar + number), progress line amber, leveling icon amber, battery blue.

- Dimmer cyan (0.26.23): volume cyan dimmed `0x07FF` → `0x03EF` (G/B halved) — the full-bright cyan was too hot.
- Mid cyan (0.26.24): tried `0x05F7` (~75%, G 47 / B 23) as an intermediate between full and half — comparing which brightness reads best.

- Unified transport icon (0.26.25): dropped the knee/leveling icon. The far-left slot is now an always-present transport indicator — **shape** gives playback state (right triangle playing / two bars paused / square stopped=no-track), **colour** gives leveling (green `COL_FOOTER_ICON` 0x05E0 normal, amber `COL_MARKER` while on). All glyphs share the 8×8 icon box. `drawLevelingIcon` → `drawTransportIcon` (reads `g_play_path`/`g_paused`/`g_leveling_enabled`); the name indent is now constant (`FOOTER_ICON_SLOT`) since the icon is always shown, simplifying the three former `g_leveling_enabled ? SLOT : 0` sites. Pause toggle already redraws the footer, so the shape tracks state live.

- Borderless bar + smaller glyphs (0.26.26): volume bar lost its outline — now just the filled column (full `FOOTER_VBAR_W` width, full footer height proportional, bottom→top). Transport glyphs shrunk `FOOTER_ICON_W` → `−2` (8→6) and centred in the slot both ways; pause bar width now scales (`(s+1)/3`) so the gap survives the smaller size.

- Red progress line (0.26.27): progress line recoloured amber → medium red `COL_PROGRESS` 0xA800 (own constant, so it no longer shares `COL_MARKER` with the leveling-amber icon) and widened 1 → 2 px (`FOOTER_PROGRESS_W`, drawn via fillRect, clamped so the 2 px span stays inside the text area).

## Conclusion

Built as a long on-hardware tuning loop — the footer was rebuilt piece by piece and judged on the device at each step, not to a fixed design.

Final footer: the track name (blue) takes the bulk of the width; volume sits on the right as a borderless vertical cyan bar (75% `0x05F7`) plus a two-char number; a 2 px red line behind the name shows track position; an always-present transport glyph at the far left shows play/pause/stop by *shape* and leveling on/off by *colour* (green/amber). The old horizontal progress bar and the separate leveling "knee" icon are both gone.

Two ideas settled the design: position is better as a thin full-span line than a slot-confined bar, and play-state reads far better from glyph shape than from a 1 px colour — which let the leveling indicator fold into the transport icon.

Scope outgrew the original "progress marker" into a full footer rework — propose archiving as `footer-rework`.

Docs: the map's Footer node was caught up (progress bar → red line, new transport icon, vertical volume bar, colours) and its `Unreviewed` warning dropped.

Shipped at **0.26.27**. Map's Footer node catch-up still outstanding for the conclusion (progress bar → line, vertical volume bar, new leveling icon, colour changes).
