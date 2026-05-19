# 8bpp row highlight broken

**Mode:** Wander

## Intent

The 8 bpp canvas change (0.17.26/28) appears to have broken the subtle dark blue-grey row highlight on the selected entry in the directory listing. The colour value (`COL_SELECTION_BG = 0x1082`) likely quantises to the same RGB332 cell as `BLACK`, so the highlight rectangle becomes invisible after rendering. Verify, then fix by either nudging the highlight colour into a distinguishable RGB332 cell, brightening it, or switching to the `palette_8bit` path with explicit colour table.

## Conclusion

Hypothesis confirmed. `0x1082` has R5=2, G6=4, B5=2 — all three channels below the RGB332 truncate-and-shift threshold (R5≥4, G6≥8, B5≥8 are the smallest values that survive quantisation). All three channels rounded to 0, producing RGB332 `0x00` = black against the black background → invisible.

Fix: nudged the colour to `0x2190` (R5=4, G6=12, B5=16) — each channel above threshold, with an extra step in blue for contrast. Now quantises to RGB332 `0x26` (dim slate-blue), visible against black. Stayed on `rgb332_1Byte`; no need to switch to `palette_8bit`.

**Changelog entry:**

> Fixed: selected-row highlight in the browser was invisible after the 8 bpp canvas change. The previous tint colour quantised to pure black under RGB332 packing, so the fill drew nothing. Replaced with a value that survives quantisation as a dim slate-blue.
