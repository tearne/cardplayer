# Frame colours by mode

**Mode:** Wander

## Intent

Reinforce the visual distinction between browse mode and search mode by recolouring the frame elements that surround the main area.

In **browse mode**: the separator bar between the header and the browser, the separator bar between the browser and the footer, and the scrollbar should all be blue (the existing slate-blue used today for the footer separator works as the canonical blue).

In **search mode**: the same three elements should all be green, matching the green theme already in place for the search prompt and selection.

## Conclusion

Added two named constants — `COL_BROWSE_FRAME = 0x6979` (the existing slate-blue) and `COL_SEARCH_FRAME = 0x652D` (a slate-green analog) — and applied them to the three frame elements in `composeBrowser` and `composeSearchView`: top separator, bottom separator, scrollbar. `drawScrollbar` (used by the directory listing) now uses `COL_BROWSE_FRAME`; the search-view scrollbar is drawn inline in `composeSearchView` and uses `COL_SEARCH_FRAME`. `COL_FOOTER_FRAME` is left in place for the footer hairline (drawn by `drawFooter`), unchanged in value but no longer doing double duty as the bottom-of-main separator.

**Changelog entry:**

> Frame elements (top/bottom separators around the main area and the scrollbar) now take the browse-mode or search-mode accent colour — slate-blue when browsing, slate-green when searching.
