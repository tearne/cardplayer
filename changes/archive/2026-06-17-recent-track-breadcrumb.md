# Recent-track breadcrumb

**Mode:** Formal

## Intent

Listening moves between folders, but the device keeps only a single global bookmark — one current folder, one cursor, and the playing track, with no intra-track position at all. Leaving folderA for folderB overwrites that bookmark, so folderA forgets where you were and you have to hunt for the track you'd reached.

The user wants each folder to durably remember the way back to the most-recently-played track in its own sub-tree, independently of its siblings and surviving power-off. Playing a track marks a breadcrumb through every folder above it, each pointing at the child that leads down toward it — so an intermediate folder with no tracks of its own still points the way down. Following the breadcrumb from any level walks straight to that track and resumes it where it left off, including the position within the track; deviating into a sibling instead reveals that branch's own most-recent breadcrumb. The remembered child stands out on sight and the cursor lands on it when the folder opens.

## Approach

### Breadcrumb stored as a hidden file per folder

Each folder holds a hidden `.cardplayer` file: a two-line text file naming the child on the path to its newest descendant track on line 1 — a subfolder for an intermediate folder, the track filename for the folder that holds it — and the playhead bytes on line 2 (`0` when the child is a subfolder). Unbounded, no per-card limit, and the data travels with the SD card and survives an NVS wipe. The file is metadata, so the browser always omits it from the listing regardless of the hide-non-audio toggle, and a pointer to a since-deleted child simply yields no breadcrumb.

### Writing the trail at play-start

Playing a track rewrites the breadcrumb file of each folder from the track up to root, each pointing one segment further down. Pointer writes happen only when a different track starts — rare relative to playback — so the steady state writes nothing to the card.

### Live position in NVS, card playhead at boundaries

The live playhead keeps saving to NVS every 10s as today — internal flash, no contention with the audio task streaming off the same card — giving crash-safe second-by-second resume of the active track. The leaf folder's card file takes the playhead only at natural boundaries (pause, track change, stop, standby), so steady playback never writes to the card. This avoids feeding the buffer-underrun problem tracked in [[track-load-underruns]].

### The existing boot bookmark is unchanged

`FOLDER`/`CURSOR`/`TRACK`/`TRKPOS` still drive boot resume of the active session. The card files are a parallel per-folder mechanism layered on top, not a replacement — boot resume of the active track reads NVS (freshest position); following another folder's breadcrumb reads that folder's card file.

### Cursor lands on the breadcrumb child

`loadDir` sets the cursor to the breadcrumb child's row instead of row 0 when the folder's file names a child still present in the listing, falling back to 0 otherwise.

### Violet marks the breadcrumb child

A new violet colour overrides `kindColour` for the breadcrumb child's row only. Its row index is resolved once when the folder opens and held alongside the listing; the row draw compares against it. The selection tint is unchanged, so on entry the row shows both violet text and the cursor tint.

### Following the breadcrumb resumes position

Playing the violet breadcrumb track resumes from its stored playhead rather than restarting. `/` on any other audio row keeps today's restart-from-zero behaviour, so resume and restart stay distinct and the violet row is the visible resume affordance.

### What updates the memory

A user playing a track from the browser records it. Auto-play-next advancing to the following track records that too — it is still "what was most recently played." Alarm fires and alarm-track picking do not.

## Plan

- [x] Read and write a folder's `.cardplayer` breadcrumb file (child name on line 1, playhead bytes on line 2)
- [x] Omit `.cardplayer` from the directory listing regardless of the hide-non-audio toggle
- [x] Write the breadcrumb trail from the played track up to root when the user plays a track or auto-play-next advances
- [x] Persist the leaf playhead into its `.cardplayer` file at pause, track change, stop, and standby
- [x] Resume from the saved playhead when the breadcrumb track is played; restart from zero on any other row
- [x] On entering a folder, resolve the breadcrumb child, place the cursor on its row, and record that row for rendering
- [x] Render the breadcrumb row in a new violet colour, leaving the selection tint intact

## Log

- `scanDir` and the fuzzy indexer both already skip dotfiles (and the indexer also requires an audio extension), so a `.cardplayer` file is filtered from both the browser listing and search with no new code — task 2 satisfied as-is.
- `parentPath`, `basename`, `joinPath` already exist; `FILE_WRITE` truncates (the fuzzy index relies on it), so the breadcrumb file overwrites cleanly.
- Boundary playhead writes go through `stopPlayback`, which fires for *any* track including alarms. To honour "alarms don't move breadcrumbs", `saveBreadcrumbPlayhead` only refreshes a folder whose stored child already equals the stopping track — it never establishes a pointer. Pointers are laid only by deliberate play (`recordBreadcrumbTrail` in the activate/skip paths).
- `jumpToPlaying` rebuilds the listing outside `loadDir`, so it now re-runs breadcrumb resolution; the resolve step was split out of cursor placement for this.
- Boot resume still restores the exact saved NVS cursor — `loadDir` now rests on the breadcrumb row, but the boot path re-applies the saved cursor over it, so the breadcrumb only governs in-session folder opens.
- The breadcrumb resume seeks while playing (not paused) to keep auto-waveform/spectrum opening; a brief blip from the track start before the seek is possible, matching existing digit-seek behaviour. Worth an ear during testing.
- Testing found the violet marker only moved on folder reopen, since `g_breadcrumb_row` was resolved solely in `loadDir`. Added `refreshVisibleBreadcrumb` after each play (activate and skip paths) to re-resolve and repaint live; it re-reads the displayed folder's file, so a track played elsewhere leaves the visible marker untouched.
- Marker colour went through several rounds with the user (violet → rose → gold on plum cursor → amber) and settled on amber `0xFD20`, reusing the browser theme's existing warm accent (`COL_MARKER`) on the unchanged blue selection cursor. A neutral selection tint behind the breadcrumb row was tried (to stop the blue making the thin amber strokes read dull) and reverted at the user's request — blue cursor on every row.

## Conclusion

Completed. Shipped at 0.28.7 (minor bump for the feature, then patches across the testing rounds). Points beyond the Log:

- Plan task "omit `.cardplayer` from the listing" needed no code — `scanDir` and the fuzzy indexer already skip dotfiles, so the file is invisible to both the browser and search even with "Hide non-audio" off.
- The amber marker reuses the browser's warm accent rather than introducing a new colour, after the colour was iterated with the user and their colleagues for on-theme fit.

**Documentation impact** — map catch-up outstanding, to handle as separate per-node negotiation:

- **Persisted State** is stale: it says "intra-track position is not persisted", but the playhead has in fact been persisted for a while; this change adds the per-folder `.cardplayer` files alongside it.
- **Browser** should gain the amber breadcrumb marker, cursor-rests-on-it, and violet-row-style resume behaviour (now amber).
- The breadcrumb mechanism may warrant its own child node.

**Proposed changelog entry:**

> ## 0.28.7 — 2026-06-17
>
> - Folders now remember where you were. Each folder keeps a breadcrumb to the most-recently-played track in its sub-tree, shown in amber with the cursor resting on it when the folder opens — follow it down to reach that track (resuming where you left off, including mid-track), or step into a sibling to see its own. The trail lives on the SD card as a hidden `.cardplayer` file per folder, so it survives a power-off, a reflash, and moving the card to another device. Playing a track or letting it auto-advance updates the trail; alarms leave it alone.
