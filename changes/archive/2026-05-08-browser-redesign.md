# Browser redesign for responsiveness

**Mode:** Formal

## Intent

The two-column 80/20 browser with its live preview pushed the device past its comfortable per-keystroke render budget. The async preview attempt (`async-preview-loading`, archived 2026-05-08) confirmed the bottleneck isn't preview I/O but the canvas-redraw-and-push cost on each cursor move. Rather than keep optimising around that constraint, this change steps back and reshapes the browser so responsiveness is the design driver, not an afterthought.

Pieces, intended together:

- **Single full-width column.** Drop the preview panel. The current directory takes the whole display width. Less per-keystroke drawing, no preview enumeration, no preview synchronisation. The "what's inside this folder" hint that the preview gave up something for is no longer worth the cost.

- **A fresh colour pass.** With no preview column to dim against, the dim/muted palette that distinguished active from preview can go. The active list gets to use its full colour space. Line separators between entries stay — they're load-bearing for visual grouping, especially when names wrap. The selection highlight gets softened — the current inverted-row treatment is louder than it needs to be once the preview's competing dimness is gone.

- **A path breadcrumb in the existing header.** A small-font line showing the current directory's full path, to maintain orientation now that the preview no longer hints at "where you are". The breadcrumb shares the header rather than growing it: the version string fades out after a few seconds at boot so the breadcrumb has room. When the path is wider than the available width, the left is statically trimmed with a `…/` prefix — newest segments stay visible. Static trim, not marquee, so we add no per-frame cost.

- **Partial canvas push.** Today `presentFrame()` pushes the whole 240×135 sprite over SPI every time. After the column drop, the SPI push is the next-largest per-keystroke cost. Pushing only the changed rectangle (the active row block, or just the row that gained/lost the highlight) is the structural responsiveness win that complements the simpler layout — together they decide whether the redesign feels merely "lighter" or actually snappy.

Map catch-up: the **Browser** node in `map.md` is currently inaccurate on at least one point — it describes "alternating row backgrounds (green-bar printer paper style)" but no such code exists in `src/main.cpp`. The redesign updates the node end-to-end as it lands, so the catch-up rides along.

The new shape is meant to feel materially snappier on the same hardware, with the colour and orientation changes folding in cleanly because the preview-column constraints they were working around are gone.

## Approach

### Single full-width column

The current 80% column simply expands to full display width; the existing scrollbar and wrap toggle (`\`) stay. The row layout is otherwise unchanged — only the column width and the absence of a preview region.

### Colours

Directory / audio / other palette stays (light blue / white / dim grey) — the dimming we had was preview-column-specific. Selection switches from inverted-row to a subtle dark-blue background tint behind the entry, leaving the foreground colours unchanged. Line separators stay.

### Breadcrumb in the header's left half, after the first keypress

Version sits top-left at boot until the user's first keypress, then fades; breadcrumb takes its place in small font. The user always sees the version on a fresh boot but it gets out of the way the moment they start interacting. Battery stays top-right. Static-trim with a `…/` prefix when wider than the slot — no marquee.

### Partial canvas push: active-row pair on cursor moves

Cursor moves push only the previously-selected and newly-selected row rectangles, not the whole browser. Full-browser push reserved for structural changes (load dir, help dismiss, redraw after pause/resume of marquee). Header gets its own push only on path change or version fade.

### Map: Browser and Header nodes rewritten as the redesign lands

Browser node is already partially stale (mentions alternating bands not in code); this change updates it end-to-end. Header node gains the breadcrumb. Per-node negotiation post-implementation per `MAP-GUIDANCE.md`.

## Plan

- [x] Remove the preview column — geometry constants, state, `refreshPreview`, render calls — and expand the active column to full display width
- [x] Selection styling: subtle dark-blue background tint replacing the inverted-row treatment; drop the `dim` rendering paths in `drawColumn`
- [x] Add breadcrumb in the header's top-left with `…/` static trim when wider than its slot
- [x] Add first-keypress version fade so the breadcrumb takes the version's slot after first interaction
- [x] Refactor cursor-move redraw to render-and-push only the row pair that changed; full-browser push reserved for structural transitions
- [x] Update `map.md` Browser and Header nodes end-to-end, post-implementation, per `MAP-GUIDANCE.md`
- [x] On-device verification: scroll feels snappy through any directory; selection clearly visible without overpowering; breadcrumb orients via left-trim with long paths; version fades on first keypress; entering / leaving dirs and the help screen still redraw correctly
- [x] Bump `APP_VERSION` from `0.14.3` to `0.15.0`

## Log

- 2026-05-08: First on-device pass after tasks 1–5 (column drop, selection styling, breadcrumb, first-keypress fade, partial canvas push). User reported "good start" but asked for clearer dir-vs-file colour distinction, dimmer unplayable text, and more subtle highlight. Adjusted palette: `COL_DIR_NORMAL` 0x3B7D → 0x4FDF (bright cyan), `COL_FILE_NORMAL` 0x6979 → 0xC618 (light grey), `COL_OTHER_NORMAL` 0x4208 → 0x2104 (dim grey), `COL_SELECTION_BG` 0x18B0 → 0x1082 (subtle dark blue-grey). All RGB565 values are first-pass; cheap to iterate.
- 2026-05-08: Map work — five nodes touched: Browser (end-to-end rewrite), Version (lifecycle clarified), Header (prose + Down link), new Path Breadcrumb subnode (with tree update), and Controls (stale-catch-up: `\` keybinding was missing from the map). The Controls catch-up rode along under the map's existing-reality-edits-anytime exemption. Per-node negotiated through chat per Engagement rule.
- 2026-05-08: One implementation deviation worth noting: the Approach proposed `…/` as the breadcrumb truncation prefix. I implemented `...` (ASCII) for font-portability, since the Cardputer's notch font handling of the Unicode ellipsis was uncertain and ASCII is guaranteed to render. Map reflects the shipped form.

## Conclusion

Shipped. Column drop plus partial-canvas push delivered the responsiveness goal; colour pass and breadcrumb folded in cleanly. Map updates and one process slip-up captured in the Log.

Final shipped version: `0.15.0`.

### Proposed `CHANGELOG.md` entry

```
## 0.15.0 — 2026-05-08

- Browser redesigned around responsiveness. The directory listing now uses the full display width (the preview panel is gone), the selected row gets a subtle dark blue-grey background tint instead of an inverted highlight, and a path breadcrumb in the header replaces the version string after the first keypress to keep you oriented. Cursor moves push only the changed row pair to the panel rather than the whole canvas, so scrolling stays smooth even through directories with hundreds of entries. Colours updated: directories bright cyan, audio files light grey, unplayable files dim grey.
```
