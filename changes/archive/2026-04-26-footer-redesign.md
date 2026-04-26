# Footer redesign

## Intent

Redesign the footer into a single information-dense row, with each piece of playback state in its own coloured slot for at-a-glance reading. Left to right: a play/pause indicator, the track name marquee-scrolling in a confined slot when too long to fit, the progress bar, and the volume bar (with the label cut down to `v:`). Each slot picks up its own colour, and the hairline framing the top of the footer becomes coloured too — the footer reads as a distinct, unified region rather than a continuation of the browser.

## Approach

### Horizontal layout

Footer height stays 16 px. Slots, left to right:

| Slot         | Width | Notes |
|--------------|------:|-------|
| Play/pause   |  ~8 px | ASCII char, vertically centred |
| Track name   | ~96 px (40%) | Marquees when wider than slot |
| Progress bar | ~96 px (40%) | Fills L→R as track plays |
| Volume bar   | ~24 px | Bar only, no text label |

Inter-slot gap of 2 px, with 1 px padding from the screen edges. The current full-width 4-px progress strip at the bottom is removed — the in-row progress bar replaces it.

### Play/pause indicator — ASCII

`>` while playing, `=` while paused (the existing chars), rendered in the play/pause accent colour. The shape itself carries the state; one colour for both keeps the slot calm. When nothing is playing, the slot is blank and the name slot reads "stopped" as today.

### Track name — marquee

When the track name fits the slot, it renders left-aligned and static. When it doesn't, it scrolls horizontally on a timer:

- Tick at ~20 Hz (every 50 ms), advancing the offset by 1 px.
- Pause for ~1 s at the start before scrolling begins.
- After scrolling fully past, pause ~1 s, then snap back to the start. (Snap-back chosen over wrap-around — simpler, and the user has already seen the full name once at this point.)
- The slot's clip rect masks the bleed at both ends.
- Marquee state belongs to the *track*, not the footer draw call: scroll offset is reset on track change, otherwise persists across redraws so an unrelated repaint doesn't cause the marquee to jitter.
- The redraw cost is local — only the track-name slot needs repainting per marquee tick, not the whole footer.

### Progress bar — in-row

Drawn within its 96-px slot at the same vertical centre as the volume bar (~6 px tall). Outline + fill in the slot's accent colour. When no track is loaded, an empty outline is drawn (rather than nothing) so the slot still reads as part of the footer composition.

### Volume bar — label-free

The `vol:` / `v:` label is dropped entirely. Position (right edge of the footer) and accent colour are expected to be sufficient to identify the bar as volume. Bar mechanics unchanged (~20 px wide, fill to `g_volume / MAX_VOL`).

### Colour scheme

Each slot's filled/drawn element picks up an accent colour; text stays white on black for legibility.

| Slot | Colour | Hex |
|---|---|---|
| Play/pause indicator | green | `0x07E0` |
| Track name | white text (no tint) | — |
| Progress bar | slate-blue (matches file-name colour in the browser, so "the track" reads as the same colour across regions) | `0x6979` |
| Volume bar | amber | `0xFD20` |
| Hairline above footer | slate-blue (same as progress bar — frames the strip with the dominant footer colour) | `0x6979` |

Shades may be tweaked on-device but the role assignments are settled. The browser's bottom separator hairline (drawn in `drawBrowser`) becomes coloured here too — it's the same hairline visually.

### Footer is always visible

The minimal-chrome toggle (`` ` ``) only hides the diagnostics row, never the footer — already true since `0.7.7`, restated here so the redesign carries that constraint forward.

### Map update — Footer node

Replace the existing `Footer` node content with:

```markdown
# Footer

[Up](#screen-layout)

A thin strip at the bottom of the display carrying — left to right — a play/pause indicator, the playing track's name (marquee-scrolling when longer than its slot), a progress bar, and a volume bar. Each slot has its own accent colour, and the hairline framing the top of the strip is coloured (same shade as the progress bar) so the footer reads as a distinct region. Always visible, independent of what the browser is showing and of any chrome-minimisation toggles. When nothing is playing, the indicator slot is blank and the name slot shows "stopped".
```

Single-node edit, executed in Build as one Plan task per `MAP-GUIDANCE.md`.

## Plan

- [x] **Add footer layout constants and accent colours.** New constants for slot positions/widths (indicator, name, progress, volume) and per-slot colours: `COL_FOOTER_PLAY` (green `0x07E0`), `COL_FOOTER_PROG` (slate-blue `0x6979`), `COL_FOOTER_VOL` (amber `0xFD20`), `COL_FOOTER_FRAME` (slate-blue `0x6979`, used for the hairline above the footer). Remove `FOOTER_PROGRESS_H` — no longer a separate strip.

- [x] **Restructure `drawFooter` to draw four slots.** Clear the footer, then in order: indicator, track name, progress bar, volume bar. Drop the full-width progress strip at the bottom. Drop the `vol:` text label.

- [x] **Play/pause indicator slot.** Print `>` if playing, `=` if paused, blank if stopped, in `COL_FOOTER_PLAY`. Vertically centred in the 16 px footer.

- [x] **In-row progress bar slot.** Outline + fill in `COL_FOOTER_PROG`, ~6 px tall, vertically centred in the 96 px slot. When no track is loaded, draw outline only (still reads as part of the layout).

- [x] **Volume bar slot.** Bar only, no text, ~20 px wide, fill in `COL_FOOTER_VOL`. Outline in same colour for visibility when fill is short.

- [x] **Track name slot — static rendering.** Render the playing track's name left-aligned in the slot, white-on-black. When nothing is playing, render `stopped`. Use a clip rect so any overflow is masked rather than spilling into the progress slot.

- [x] **Track name slot — marquee state and tick.** New struct holding scroll offset, phase (pre-pause / scrolling / post-pause), phase-start `millis()`, and the track path the state belongs to. Add `pollMarquee()` running on the main loop at ~20 Hz; advances state, redraws only the name slot when the visible content changes, resets on track change. Skipped while `g_show_help` is set (don't disturb the help overlay).

- [x] **Recolour the browser-to-footer hairline.** In `drawBrowser`, change the bottom-edge separator from `COL_HEADER_TXT` to `COL_FOOTER_FRAME`. The top separator (browser-to-header) keeps its existing colour.

- [x] **Update the Footer node in `map.md`.** Replace the node's body with the rewritten text from the Approach (`### Map update — Footer node`).

- [x] **Test on device.** Verify slot layout reads cleanly; play/pause indicator transitions across `>` / `=` / blank as expected; long names marquee with the right pacing; short names sit static; progress bar fills as track plays and shows empty outline when stopped; volume bar tracks `g_volume` without a label; hairline above the footer is the new colour; minimal-chrome toggle still doesn't hide the footer; help overlay isn't disturbed by the marquee tick.

## Log

- `0.8.1`: first test handover. All implementation tasks done; only on-device verification remains.

- `0.8.2`: play/pause indicator slot dropped on user feedback (didn't earn the space). Progress bar's colour now carries the playing/paused state — slate-blue while playing, mid-grey while paused or stopped (`COL_FOOTER_IDLE = 0x7BEF`). Reclaimed space goes to the name slot (`FOOTER_NAME_X = 1`, `FOOTER_NAME_W = 106`). Volume bar recoloured from amber to teal (`0x07F8`) per user preference for a cooler hue. Map node updated to match.

- `0.8.3`: footer height halved from 16 px to 10 px — only one line of content remains (text + bars are 8 / 6 px tall), so the second 8-px row was wasted vertical space carried over from the old two-row layout (text row + separate progress strip). Browser gains the 6 reclaimed pixels.

- `0.8.4`: volume bar colour pulled back from the very saturated `0x07F8` to a muted teal `0x34D0` (~RGB 48,152,128) per user — the original read as fluorescent next to the calmer slate-blue progress bar.

## Conclusion

Completed. Notable deviations from the Approach, each driven by on-device feedback after the first handover:

- **Play/pause indicator slot dropped** (`0.8.2`): the Approach allocated a dedicated ~8 px slot for `>` / `=`. User judged it didn't earn the space; the progress bar's colour now carries the play/pause state (slate-blue when playing, mid-grey when paused or stopped). Reclaimed pixels went to the name slot (now 106 px wide).
- **Footer height halved** (`0.8.3`): the Approach assumed the existing 16 px footer; with everything on one row, the lower 8-px band was empty space carried over from the old two-row layout. Reduced to 10 px; browser gains the 6 reclaimed pixels.
- **Volume bar colour** iterated through three picks: the Approach proposed amber, user asked for green/blue → teal `0x07F8` (`0.8.2`), then asked for it duller → muted teal `0x34D0` (`0.8.4`).

Map's Footer node was updated as planned. The marquee behaviour and overall per-slot colour scheme landed as designed.

**Documentation impact**: none beyond the Footer node. The Controls map node still has stale references to `<` / `?` track-skip and pre-`ui-ergonomics` keybindings — that carryover predates this change and remains pending its own plan-mode pass.
