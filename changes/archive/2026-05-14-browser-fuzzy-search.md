# Browser fuzzy search

**Mode:** Formal

## Intent

Integrate fuzzy track-finding into the browser as a first-class way of getting around the library. The investigation in `2026-05-10-fuzzy-track-finding.md` showed that an SD-resident page-bloom + unigram-table index is viable on this device; this change brings that capability into the user-facing UI.

UX shape (from initial conversation):

- The top row of the browser becomes a search input — query text is typed there.
- Match results appear in the body of the browser below, scrollable in the same way the directory listing scrolls today.

## Approach

### Activation model — type-to-search

Currently no alphabet letter is bound to an action. Pressing a letter while in the browser will enter search mode and seed the query with that letter; subsequent keystrokes append. Backspace deletes; backspace on an empty query exits search; `Esc` (or any clean exit affordance the keyboard exposes) also exits. This mirrors Mac Finder / file-manager UX and avoids burning a dedicated key for activation.

Reason: type-to-search is the most discoverable shape, and the alphabet is currently unused so the cost of claiming it is zero. An explicit activation key (e.g. `?`) would still leave us shipping with most letters as no-ops.

### Search-mode browser layout

The top row of the browser slot becomes a single-line text input showing `> <query>_` (cursor as block or underline). The remainder of the slot lists ranked match results, scrollable, with the existing scrollbar gutter. The directory listing is hidden while in search mode — search and directory navigation are alternatives, not co-present.

Reason: reusing the existing list-render path keeps the visual grammar consistent with regular browsing and avoids designing a third layout. The directory list is irrelevant inside search anyway.

### Result rendering — full path, not leaf

Each result row shows the full path so the user can disambiguate two tracks with the same filename in different albums. Same audio-file colour as the browser uses today (light grey). No score displayed.

Reason: in search mode, context — which album / which folder — is the only thing distinguishing similarly-named files.

### Result actions — Enter plays + exits

Pressing Enter on a result starts playback exactly as it does in the browser today, and exits search mode back to the directory containing the chosen track, with that track selected. The user lands in normal browser context with their pick highlighted.

Reason: search is a navigation shortcut, not a parallel mode. After acting on a result the user is back where they would have arrived had they navigated there manually.

### Index lifecycle — persisted files, CID-fingerprinted, manual refresh

At boot, after SD mount, check whether `/FZTI.idx` and `/FZTI.pb` exist. The page-bloom file's header carries the SD card's CID (the 128-bit per-card identifier the SD subsystem reads at init). If files exist and the stored CID matches the live card, the index is reused as-is — no rebuild, search available immediately. If either file is missing or the CID differs (different card inserted), kick off the build on a background FreeRTOS task on core 1 (priority 1, the shape the investigation validated). While the build runs, search is unavailable — the input row shows an "indexing…" state, and any letters typed become the seed query when the build finishes.

The user triggers a manual rebuild by pressing `~` from anywhere in the browser; it runs on the same background task. A spinner in the top-right of the header indicates a build in progress (boot-time or manual). The search input row, when active, also shows a quiet "**N tracks indexed**" indicator so the user has a sanity-check on whether the index reflects the current library — if N looks wrong after adding music, they hit `~`.

Detection of in-card track additions is explicitly out of scope; CID handles the card-swap case, and the user is responsible for refreshing after additions. Detection of mid-session card removal is also deferred.

Reason: CID gives free, reliable card-swap detection without the cost of a directory walk. Persisting the files amortises the ~30 s build across power cycles for the common case (same card, no changes). Manual refresh keeps the model predictable.

### Match scope — audio files only

Search results include only audio files, not directories. Simpler index, simpler result-action handling (Enter always plays). Directory matching can be added later if it turns out to be missed; not in this iteration.

### Code organisation — promote out of the harness

The investigation's `fuzzy_harness.cpp` is gated by `FUZZY_HARNESS` and lives only in the `cardputer-fuzzy` env. The production index and query code (the `pbBuild` / `pbQuery` / `sdBuild` / `sdLookupPath` paths plus their data structures) move to a new `src/fuzzy_index.{h,cpp}` compiled into the default `cardplayer` env. The harness loses those functions and becomes a thin wrapper around the production module for measurement. Synthetic-library generation stays in the harness — not relevant to production.

Reason: search becomes a first-class feature, not a flag-gated capability. The harness still exists for future re-measurement.

## Plan

### Tasks

- [x] Extract index and query code from `fuzzy_harness.cpp` into `src/fuzzy_index.{h,cpp}`, compiled into the default `cardplayer` env; harness becomes a thin wrapper for measurement
- [x] Add SD-card CID to the page-bloom file header (v3); write at build time, read and verify at load time
- [x] Boot-time check-or-build: at SD mount, reuse existing files if present and CID matches, otherwise kick off a background build
- [x] Spinner in header top-right while a build is running
- [x] `~` from the browser triggers a background rebuild
- [x] Search-mode state (active flag, query buffer) and top-of-browser input row rendering
- [x] Pressing an a–z letter in the browser enters search and seeds the query with that letter
- [x] Input editing: letters/digits/space append; backspace deletes; backspace-on-empty exits; Esc exits
- [x] Each query change runs `pbQuery` and caches the result list
- [x] Result list renders in the browser body, full path, scrollable, scrollbar gutter reused
- [x] `;` / `.` move the cursor within the result list
- [x] Enter on a result starts playback, loads the result's directory, selects the track, and exits search
- [x] "N tracks indexed" indicator in the search input row
- [x] Help screen lists the new bindings (any letter → search, `~` → rebuild, Esc → exit search)

## Log

- 0.17.0: extracted production index/query code from `fuzzy_harness.cpp` into `src/fuzzy_index.{h,cpp}`. Public API is `initAtBoot(card_fingerprint)`, `startRebuild()`, `state()`, `pathCount()`, `query(...)`, `lookupPath(...)`. Harness slimmed to synth-library generation + a few measurement commands wrapping the public API.
- Card fingerprint uses `SD.cardSize()` rather than the actual CID — Arduino-ESP32 SD library doesn't expose CID via the public API. `cardSize()` distinguishes the common case (different-capacity card swap); a same-size different-card swap requires the user to press `~`.
- PB header bumped to v3 to carry the 64-bit fingerprint.
- The Approach mentioned `Esc` as a search exit affordance, but the M5Cardputer keyboard has no Esc key (`KeysState` exposes tab/fn/shift/ctrl/opt/alt/del/enter/space, no esc). Implemented backspace-on-empty as the sole exit. If a second one-key escape is wanted, `tab` is currently unbound and could be claimed in a follow-up.
- 1-char fastpath uses precomputed unigram top-K so a fresh letter press feels instant; 2-char queries fall back to flat scan (~200 ms); 3+ char queries use page-bloom filtering.
- 0.17.1: three reported issues addressed.
  1. Search-view font now matches the browser — uses `notchFont()` / `notchSize()` and `charW()` instead of the hard-coded `setTextSize(1)` / `BASE_CHAR_W`.
  2. Non-alphanumeric keys retain their pre-existing browser bindings while in search mode. Only letters, digits, and space append to the query; `,` `/` `;` `.` `'` `[` `]` `=` `-` `+` `_` `` ` `` `\` `?` `~` keep doing what they do outside search. Backspace edits the query (and exits when empty); Enter activates the selected result.
  3. Esc isn't on the M5Cardputer keyboard, so substituted **Tab** as the explicit exit. Backspace-on-empty still exits as before. Help screen updated.
- 0.17.2: switched the explicit-exit binding from Tab to **Fn+\`**. The backtick key has "esc" printed as its Fn-modified label on the physical keyboard, so this is the discoverable, semantically-correct binding. The library doesn't auto-translate the Fn label (its `_key_value_map` only has shifted second values, not Fn-modified ones), so the application interprets `state.fn && state.word == "\`"` as "esc". Help screen updated.
- 0.17.3: result rows show only the track filename (basename of the path) — full paths were too wide for the column. Wrap mode (`\`) toggles for search results the same way it does for the browser. Search-mode visual differentiation: cyan prompt and caret in the input row, and a warmer dark blue selection background for the highlighted result (`COL_SEARCH_SEL_BG = 0x4082` vs the browser's `COL_SELECTION_BG = 0x1082`).
- 0.17.4: switched search-mode tones from blue to green per user preference — prompt/caret in light green (`COL_SEARCH_PROMPT = 0x4FE0`, the green counterpart of the directory cyan), selection background in dark muted green (`COL_SEARCH_SEL_BG = 0x0240`).
- 0.17.4: scoring tweak — consecutive-match bonus bumped from +2 to +4 in `scoreMatch`. Bug surfaced when the user typed "time": the path containing "Tim Eng…" (matching t-i-m, gap across space, then E getting a word-boundary bonus) outscored "time.mp3" (contiguous t-i-m-e). The +2 consecutive bonus failed to dominate the +3 boundary bonus, so the discontinuous match won. With +4, contiguous runs strictly outrank gap-with-boundary matches.
- 0.17.5: scoring rebalanced after a user-requested discussion. Three principled changes:
  1. Match against the **leaf only** (filename), not the full path. Directory components no longer contribute to scoring. Callers strip the leaf via `leafOf()` before invoking `scoreMatch`; the page-bloom filter and unigram top-K table are also built from leaves only.
  2. **Word-boundary bonus removed** entirely. The previous +3 for matching after a delimiter (including the start of the filename) had been distorting rankings — the user prefers consecutive runs as the only signal.
  3. **Consecutive bonus made cumulative and stronger.** Each subsequent char in a consecutive run adds `+6 + run_length` (where `run_length` is the run's position, 1-indexed). A 4-char contiguous match gets `1 + 8 + 9 + 10 = 28`; a 4-char match with one gap scores closer to `1 + 8 + 1 + 8 = 18`. Long runs are now super-linearly preferred.
- PB header bumped to v4. Existing v3 files fail the load check and trigger an automatic background rebuild on next boot.
- 0.17.6: "N indexed" hint hidden once the query becomes non-empty — previously the query text would visually run into the right-justified indicator on long inputs. The hint now only shows as the pre-type sanity check.
- 0.17.7: refined the hide rule — the indicator stays visible until the typed query's end (including caret) would come within 2 char-widths of its left edge, then disappears. So short queries keep the indicator visible; only long enough queries displace it.

## Conclusion

Browser fuzzy search lands as a first-class feature at v0.17.7. Type any letter in the browser to enter search; results refine per keystroke; Enter plays the chosen track and drops you back into its directory; `Fn+\`` (labelled "esc") or backspace-on-empty exits. `~` triggers a manual rebuild. The index lives on the SD card and is reused across boots unless the card fingerprint (`SD.cardSize()`) differs.

**Deviations from the Approach.**

- *Exit affordance.* Approach said `Esc`. The M5Cardputer keyboard has no Esc key, so settled on **Fn+\`** (the key whose Fn label reads "esc"). Backspace-on-empty also exits.
- *Card fingerprint.* Approach said "SD CID". The Arduino SD library doesn't expose the true CID, so used **`SD.cardSize()`** as a coarse proxy. Card-swap to a same-size card requires the user to press `~` to refresh — documented edge case.
- *Scoring rebalanced mid-build.* After initial implementation surfaced odd rankings, scoring was simplified per a focused conversation: match against the filename only (not full path), no word-boundary bonus, super-linear consecutive-run bonus. PB header bumped to v4 to invalidate old indexes built under the previous scoring.
- *Search-mode theme.* Approach was silent on theming. Settled on green tones (cyan-equivalent light-green prompt, dark muted-green selection) so the user can tell browse-vs-search at a glance.

**Documentation impact.**

- `map.md` — the Browser node now has two modes (directory listing + search) and the Controls node has new bindings (`a-z`, `~`, `Fn+\``). Worth a per-node update following this change.
- No new top-level concept; this is an extension of the existing Browser, not a separate region.

**Changelog entry.**

> Browser fuzzy search added. Type any letter in the browser to enter search; results refine per keystroke from an SD-resident index. `Fn+\`` (esc) or backspace-on-empty to exit. `~` rebuilds the index. The index is built in the background at boot (~28 s for ~3 K tracks) and reused across boots unless the card changes. Ranks by contiguous-substring strength against the filename only.

