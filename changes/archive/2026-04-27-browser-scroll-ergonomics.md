# Browser scroll ergonomics

## Intent

Two adjacent friction points when moving through long folders in the browser:

- The browser gives no indication of how far down a folder the highlight is, or how much remains. A scrollbar alongside the directory column shows position and extent at a glance.

- Up/down stepping is one keypress per row. Holding the up or down key should scroll continuously, so traversing a long folder doesn't require repeated tapping.

## Approach

### Held-key cursor movement

Mirror the existing `pollSeekKeys` pattern — a poll function called every loop iteration that inspects `keysState().word` for `;` or `.` and steps the cursor at a uniform 100 ms cadence while either key is held. The current `;`/`.` cases in the `isChange()` switch are removed so initial press and subsequent repeats both go through the same path, avoiding double-fire on the first frame.

### Scrollbar placement

A 3 px gutter on the right edge of the active (left) column, just inside the existing column divider line. The divider stays as it is. The gutter is always reserved so the content layout doesn't shift when entries are added or removed; the thumb is drawn into it only when entries overflow.

The active column's effective content width shrinks by the gutter, so wrap calculations (`charsPerLine`, `entryRows`) and the clip rect both account for it — the thumb never overpaints a name.

The gutter track is left blank (background). The thumb is drawn in `COL_HEADER_TXT` — the same dim slate-blue used for the column divider and other browser chrome.

### Scrollbar semantics

Viewport-style: the thumb covers the range of currently visible entries within the total list. Top of thumb = `g_top / total`, length = `visible_count / total`, both scaled to gutter height. `visible_count` is the number of entries rendered before the column ran out of room — `drawColumn` already computes this loop-by-loop and can return it. A minimum thumb length keeps the thumb visible on very long folders.

### Map

The affected nodes are **Browser** (column layout gains the gutter / scrollbar; behaviour during long folders) and **Controls** (`;` / `.` semantics extend to held-repeat). Map catch-up is a per-node negotiation after the build.

## Plan

- [x] Add `pollBrowserNavKeys()` modeled on `pollSeekKeys` — inspects `keysState().word` for `;` / `.` and calls `moveCursor(±1)` at a uniform 100 ms cadence while either is held.
- [x] Call `pollBrowserNavKeys()` from `loop()`, alongside `pollSeekKeys()`.
- [x] Remove the `;` / `.` cases from the `isChange()` switch in `loop()` so initial press and held-repeat both go through the poll.
- [x] Add a `SCROLLBAR_W` constant (3 px) and use it to reduce the active column's effective content width — both the clip rect and the `col_w` value used for wrap calculations in `drawColumn`'s active path.
- [x] Make `drawColumn` report the count of entries it rendered before running out of room (return value or out-param).
- [x] In `drawBrowser`, when the active column overflows (`visible_count < total`), draw the scrollbar thumb in the reserved gutter — top = `g_top / total`, length = `visible_count / total`, both scaled to browser body height, with a minimum length of a few pixels. Colour `COL_HEADER_TXT`.
- [x] Bump `APP_VERSION` from `0.9.0` to `0.10.0`.
- [x] On-device test: thumb position and length track navigation through a long folder; held `;` / `.` scrolls continuously without stutter; wrap-names toggle still wraps correctly with the gutter present; preview column and divider unchanged.

## Log

- On-device test showed that the planned uniform 100 ms cadence double-fired on a normal tap (key dwell exceeds 100 ms). Switched to the keyboard-style shape the Approach had listed as an alternative: fire once on press, wait 400 ms, then repeat every 100 ms. Bumped to `0.10.1`.

## Conclusion

Completed. The cadence deviation during testing is in the Log. Map catch-up for the Browser and Controls nodes is pending and will be handled as per-node negotiations.
