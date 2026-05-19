# Empty-folder font

**Mode:** Wander

## Intent

`composeBrowser` renders "(empty)" with `setTextSize(1)` regardless of the user's current font notch — looks tiny vs the rest of the browser content. Use the notch font so it matches.

## Conclusion

Swapped `setTextSize(1)` for `setFont(notchFont())` + `setTextSize(notchSize())` in the empty-folder branch. Matches every other browser render path.

No changelog entry — cosmetic fix, not user-tracked.
