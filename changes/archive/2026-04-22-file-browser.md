# File browser

## Intent

Let the user browse the microSD card and pick something to play, instead of having playback hardcoded. The relationship between the browser and playback (whether it stays visible, how the user returns to it, etc.) is left open for the Approach stage.

## Approach

Turn the flat root-only list into a multi-column browser in the style of macOS Finder column view. The main area is split into two panels: the **current directory** on the left and a **preview** of the highlighted entry (if it's a directory) on the right. Pressing right descends into the previewed directory; pressing left steps out to the parent.

This change also minimally realises the map's **Footer**, pulling the existing bottom status line, progress bar, and volume indicator into it. Without the Footer, playback feedback would vanish as soon as the user navigated away from the playing track's folder.

**Columns.** 80/20 split — current directory on the left (192px), preview on the right (48px). The preview shows the contents of whichever directory is highlighted in the current column, rendered dimmer to hint it isn't interactive yet. Entries in the preview column may only show a leading fragment of their names.

**Entries.** Directories first, then audio files, then non-audio files, alphabetical within each group. Colour palette:

- Directories: light blue (`0x5D9F`)

- Audio files: white

- Non-audio files: dim grey (`0x4208`) — shown, not hidden, so the user can see what's on the card

Long names wrap to a second (or further) line within the column — no truncation. Each entry is backed by an alternating row tint inspired by green-bar printer paper: black and a very faint green-tinged dark tone (`0x0120`), so a wrapped name visibly stays grouped with itself. The selected entry in the active column overrides the tint with a bright highlight (inverted text). The preview column renders everything at roughly half brightness and does not draw a selection highlight.

**Navigation.** Vertical selection in the current column with `;` / `.` (up/down arrows) as today. `/` (right arrow) enters the highlighted directory — current column shifts left, preview becomes the new current. `,` (left arrow) steps out to the parent. `enter` on a file plays it.

**Skip behaviour.** Track skip moves to `<` / `?` (shifted left/right) to free the arrows for directory navigation. Skip operates within the directory the *playing* track belongs to, not the one being browsed. At the end of that directory, playback stops.

**Footer.** A thin strip at the bottom carrying track name, play/pause state, progress bar, and volume. Always visible, independent of where the user has browsed to. The existing `STATUS_Y` / `PROGRESS_Y` elements move into it.

### Map edits

**New node — Browser (child of Screen Layout):**

```markdown
# Browser

[Up](#screen-layout)

Fills the main area in a two-column 80/20 style. The left column shows the current directory; the right column previews the highlighted entry's contents, rendered at roughly half brightness as a hint that it isn't the active column yet. Directories first, then audio files, then non-audio files, alphabetical within each group. Directories are light blue, audio files white, non-audio files dim grey. Long names wrap within the column. Entries are tinted with alternating row backgrounds (green-bar printer paper style — black alternating with a faint green-tinged dark tone) so wrapped names stay visibly grouped. Remains visible during playback.
```

**Updated node — Footer (fleshed out now that it's realised):**

```markdown
# Footer

[Up](#screen-layout)

A thin strip at the bottom of the display carrying track name, play/pause state, a progress bar, and the volume level. Always visible, independent of what the browser is showing. When nothing is playing, shows "stopped".
```

**Updated node — Screen Layout (adds Down link to Browser):**

```markdown
# Screen Layout

[Up](#application)
[Down](#header)
[Down](#browser)
[Down](#footer)

The display is divided into three fixed regions: a header strip at the top, a footer strip at the bottom, and a main area in between. The main area hosts the browser. The header and footer are always present.
```

**Updated node — Controls (adds directory-nav bindings):**

```markdown
# Controls

[Up](#application)

Everything the user does is via the Cardputer's keyboard — there is no touch or rotary input. The current bindings:

- `;` / `.` — move selection up / down

- `,` / `/` — step out to parent / enter highlighted directory (left / right arrow)

- `<` / `?` — skip to previous / next track within the playing directory

- `enter` — play the selected track

- `space` — pause / resume

- `-` / `=` — volume down / up
```

**Tree overview in the root is unchanged structurally** — Browser is added under Screen Layout as a sibling to Header and Footer. The tree already showed Footer; the difference is it's now actually implemented:

```
Application
├ Screen Layout
│ ├ Header
│ │ ├ Version
│ │ └ Battery
│ ├ Browser
│ └ Footer
└ Controls
```

## Plan

- [x] Replace the flat `scanRoot()` with a directory-aware scan that lists one path's subdirectories and files, classified as directory / audio / non-audio, sorted dirs → audio → non-audio, alphabetical within each group.

- [x] Introduce browser state: current path, selection index within it, and — separately — the playing track's full path and the directory it belongs to.

- [x] Reserve a fixed Footer region at the bottom and move the existing status line, progress bar, and volume indicator into it. The Browser area is then a defined rectangle between Header and Footer.

- [x] Render the current directory in the left 80% of the Browser area with line-wrapped entry names and alternating green-bar zebra tinting; draw the preview (highlighted entry's contents if it's a directory, else empty) in the right 20% at roughly half brightness.

- [x] Apply entry colouring: directories light blue, audio files white, non-audio files dim grey. Selected entry in the active column drawn inverted.

- [x] Rebind navigation: `,` / `/` step out of / descend into directories (consumed by browsing, not track skip); `<` / `?` now carry track skip. `enter` plays when the selection is a track; no-op on a directory. Existing `;` / `.` / `space` / `-` / `=` unchanged.

- [x] Track skip operates within the playing track's directory (not the browsed directory); stops at either end rather than wrapping.

- [x] Add the **Browser** node to `map.md` as a child of Screen Layout.

- [x] Update the **Footer** node in `map.md` to reflect its realised content (track, transport, progress, volume).

- [x] Update the **Screen Layout** node in `map.md` to add `[Down](#browser)`.

- [x] Update the **Controls** node in `map.md` for the new key assignments.

- [x] Update the root tree overview in `map.md` to include Browser.

## Conclusion

- Non-audio files are displayed when they are siblings in a directory; hidden dotfiles are still skipped. The Approach said "shown", which is honoured.

- Hidden files (names starting with `.`) are filtered out — macOS etc. can leave `.DS_Store` and similar on the card. Small call not captured in the Approach; worth noting.

- Playback auto-advance on natural track end was not added — the PoC stopped on finish and that behaviour is preserved. Skip via `<`/`?` is the only way to move between tracks. If auto-advance is wanted later, it's a small follow-up.

- Known issue: SD directory scans during navigation briefly interrupt audio. The main loop is single-threaded and scanning blocks audio decoding long enough for the output buffer to drain. Tracked as a separate change (`glitch-free-audio`) rather than expanding scope here.

- Not yet verified on-device.

