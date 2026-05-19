# Help screen resize

**Mode:** Wander

## Intent

Make the help screen render at the same font size as the browser and the search results (the user's current notch font), rather than the smaller fixed `Font0 size 1` it uses today. The bigger font won't fit all help lines on screen at once, so add a scroll affordance using the existing `;` / `.` keys. Layout is also free to change for simplicity now that vertical scrolling exists.

## Conclusion

Help renders at the notch font now, with `;` / `.` to scroll and a scrollbar in the gutter when the list overflows. Layout restructured to a two-column "definition list" — key in a left column, description in a right column starting at a fixed indent — so descriptions that wrap continue at the description column rather than back to the left margin. Section headers (Browse / Playback / Adjust / Misc) get a dim grey background strip to read as visual breaks. Hairline above every entry except the very first visible row, matching the browser's row separators. Descriptions capitalised.

**Changelog entry:**

> Help screen redesigned: now uses the same font size as the browser, scrolls with `;` / `.`, and re-flows wrapped descriptions to align under the description column instead of back to the left margin. Section headers get a grey background and hairlines separate entries.
