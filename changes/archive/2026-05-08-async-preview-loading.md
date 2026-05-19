# Async preview-column loading

**Mode:** Formal

## Intent

Stepping through directory entries in the browser should feel responsive regardless of how big each entry's folder is. Today it doesn't: when the highlight lands on a directory containing lots of audio files, the UI stalls visibly while that folder's contents are listed to populate the preview column on the right. Holding the down/up keys to scroll past a stretch of large folders chains those stalls together — the scroll feels sluggish or unresponsive in proportion to the directories' size, not the number of keypresses.

The preview is a hint, not the active column. Its job is to give the user a sense of what's inside the highlighted directory; users don't need to see every track immediately to keep navigating. The currently-selected highlight, the cursor advancing under their key press, and the active column's render — those are the things that need to stay snappy. Loading and rendering the preview content can happen *after* the scroll settles, off the critical path.

The change is to move the preview column's directory listing off the synchronous selection-change path so scrolling stays responsive even through directories with hundreds of tracks. Once the user pauses on a selection, the preview catches up.

## Approach

### Worker task off the main loop

A small dedicated FreeRTOS task takes preview-build requests off-main-loop. When the selection changes, the loop posts the new directory to the worker; the worker reads SD, sorts, and publishes the result. The main loop never blocks on SD even for a single folder.

### Cancellation by monotonic generation

A `g_preview_gen` counter increments on every selection change. The worker captures it at job start, does the work, and only publishes the result if the generation still matches at completion. Stale work is discarded silently. No queue depth, no settle window — the next selection change supersedes the previous unconditionally.

### SD access serialised via the existing audio mutex

`g_audio_mutex` already serialises audio decode (held during `g_gen->loop()` which reads SD internally) and the main loop's `scanDir()` calls. The preview worker takes the same mutex around its enumeration; no new lock, no audio-side changes.

### Cap the enumeration

The preview is a hint, not a full listing — the column shows ~12 rows and large folders aren't navigable here anyway. The worker stops reading directory entries after a cap (~50) to keep mutex hold-time bounded and audio starvation impossible. If truncated, a trailing `…` row signals more entries exist. Caps the worst-case audio impact at well under the speaker DMA queue's `~186 ms` headroom.

## Plan

- [x] Add a capped variant of `scanDir` — reads at most N entries, signals whether the directory had more
- [x] Create the preview worker task with notification, monotonic generation counter, and a published-result slot under a small mutex
- [x] On selection change, bump the generation and notify the worker
- [x] Worker body: take the audio mutex, capped-enumerate, release the mutex, publish result if generation still matches at the end
- [x] Replace the synchronous preview-column `scanDir` call with a worker-result lookup; render a faint `…` row when the published generation lags the current selection
- [ ] On-device verification: scrolling through a directory of large folders stays smooth; audio plays without underruns while previewing; truncation `…` appears for folders past the cap; in-flight `…` appears during the gap; existing UI behaviour unchanged

## Log

- 2026-05-08: First on-device test of pure-async (no settle, no redraw trigger) revealed three issues. (1) Single-subdir previews never showed — the worker published but no event triggered the UI to redraw, so the placeholder stuck. (2) Scrolling jittery and unresponsive across large dirs — back-to-back enumerations during continuous scroll hammered the SD bus and the worker shared core 1 with the UI loop at the same FreeRTOS priority, time-slicing draw cycles. (3) Keypresses appeared to buffer and replay on settle — same root cause as (2), the loop starved while the worker chewed. Resolution: added an 80 ms settle window (loop notifies the worker only after the latest selection has been quiet that long) and a per-loop redraw trigger when the published gen advances. The "deferred vs async" framing the user pushed back on earlier was correct in spirit — the work IS still off-thread; the settle just prevents wasted enumerations during continuous scroll. Both fixes together; not yet retested on hardware.
- 2026-05-08: Second on-device test surfaced phantom files — folder contents appearing as the preview of selected *files*, plus persistent slowness/buggy feel. Root cause was a single missed cleanup: the synchronous non-dir publish (and the empty-cursor branch) cleared `g_preview` and bumped the generation but did not clear `g_preview_request_path`. If `pollPreviewDispatch` subsequently saw the new generation as still pending against the stale path, it fired the worker for the previous directory; the worker enumerated and published its contents at the new generation, which the render path accepted as current. Fixed by clearing `g_preview_request_path` in both synchronous-publish branches.
- 2026-05-08: Third on-device test still showed unsatisfactory responsiveness. Diagnosis: the async preview architecture is correctly removing preview I/O from the per-keystroke critical path (settled, capped, cancellable, no longer holding the mutex during continuous scroll), but scrolling responsiveness *still* doesn't feel snappy because the dominant per-keystroke cost isn't the preview — it's the canvas redraw itself. Each cursor move re-renders both columns and pushes the entire 240×135 sprite to the panel over SPI. No amount of off-thread preview work addresses that. The browser style itself is the responsiveness ceiling on this hardware.

## Feedback

**Status:** not implemented.

**Notes:** the async refactor is technically correct — preview enumeration is now off-thread, capped at 50 entries, cancellable via monotonic generation, and dispatched only after an 80 ms settle window. The phantom-files race was found and fixed. But three rounds of on-device testing showed the user-perceived responsiveness barely improved over the synchronous version, because the change addresses the wrong bottleneck. The remaining cost is the per-keystroke full-canvas redraw + SPI push to the panel, not preview I/O.

Game-changers within "two-column browser with live preview" all amount to refactoring the rendering path itself — partial canvas updates, row caching, simpler row layout. The user judged it cheaper to step back and rethink the browser style entirely.

**Documentation impact:** the Browser node in `map.md` (two-column 80/20 layout, live preview at half brightness) is the design under review. Successor change should propose its own browser-style decision with the map node updated accordingly.

## Conclusion

Aborted before shipping. All code reverted; firmware stays on `0.14.3`. The architectural pieces (worker task, capped scanDir, generation-based cancellation, settle window) are sound but not enough — the perceived-responsiveness ceiling on this device/UI combination is the canvas redraw, not the preview I/O. Superseded by a new change reframing the browser around responsiveness as the primary constraint.

No `CHANGELOG.md` entry — no shipped firmware change.
- [x] Bump `APP_VERSION` from `0.14.3` to `0.14.4`

### Visual during gap: faint `…` placeholder

While a new selection has been made and the worker hasn't yet published a fresh result, the preview column shows a single faint `…` row rather than the previous selection's listing. Hints that loading is in progress without misleading about the actual contents.
