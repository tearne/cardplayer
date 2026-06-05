# NVS inspection

**Mode:** Formal

## Intent

Persisted state lives in NVS, but there's no way to see what's in there without Espressif tools.

The user wants to, from Settings, (a) see how full the NVS partition is, and (b) view every key physically present, so stale keys left behind by old firmware versions can be spotted against the set the current build actually uses — and then delete the stale ones, without a full reset.

## Approach

### A "Settings data" action row opening a scrollable sub-screen

A new Settings action row, *Settings data*, opens a full-screen scrollable list, Esc back to Settings — the Key Reference shape. Being in Settings, it ships in every build with no separate flag.

### Discover keys with the raw NVS iterator; measure fullness with stats

`Preferences` can't enumerate keys, so the view uses the ESP-IDF `nvs.h` API: `nvs_get_stats` for (a), `nvs_entry_find`/`_next`/`_info` to walk every namespace+key for (b). The iterator yields namespace, key, and type; the value is then read by type and shown — key left-justified, value right-justified on the following line.

### Stale detection against a single shared key list

Each namespace owns the single definition of the keys it writes — the `player` keys centralised where save/load reference them (the per-slot alarm keys via one shared generator), the `chess` keys owned by the chess module. A present key is *stale* when absent from its owner's list, or in a namespace no owner claims. save/load reference these shared definitions instead of inline string literals, so the writer and the auditor can never disagree.

### A single "delete all stale" action, guarded by a confirmation modal

The only destructive affordance erases every stale-flagged key at once. It opens a confirm modal (Reset Confirmation precedent) naming the count, then erases each via `nvs_erase_key` on an open handle plus commit. No per-key delete — the list stays a read-only scroll plus one bulk action.

### Capacity shown as used vs total entries

The summary line renders `nvs_get_stats` as used / total entries plus a percentage. Entries are 32-byte slots, not keys, so this reflects real partition pressure.

## Plan

- [x] Define the `player` key names once as shared constants, including a shared generator for the per-slot alarm keys
- [x] Route player save/load through those shared definitions in place of inline string literals
- [x] Expose the `chess` namespace's expected keys from the chess module
- [x] Add a Settings-data screen reached from a new "Settings data" action row, Esc back to Settings
- [x] Show the capacity summary from `nvs_get_stats` (used / total entries + percentage)
- [x] List every NVS entry via the iterator with its key and typed value, blobs as a byte count, scrollable with the existing scrollbar treatment
- [x] Mark each entry stale when its key is absent from its namespace's shared list, or its namespace is unclaimed
- [x] Add a "delete all stale" action, guarded by a confirmation modal, that erases each stale key and refreshes the list

## Log

- The `chess` namespace writes six keys (`board`, `side`, `castle`, `ep`, `last`, `diff`), not the three the map's Persisted State node mentioned — the map is stale here and is a catch-up candidate.
- Confirmed `resetState()` clears only the `player` namespace, so "Reset all" never touches chess keys. The stale-audit is the only in-app way to remove orphaned chess keys.
- Delete-stale confirm uses `/` to confirm, matching Reset all's modal idiom (Enter opens it from the list; `/` commits in the modal).
- Builds clean for `cardplayer`; flash 28.5%, RAM 23.8%. Not yet run on device.
- Feedback after first build: reworked the flat list into a two-layer view — a top menu (Stale count / Current count / Delete all stale) with each count opening a scrollable sub-list. Long keys/values now wrap across rows; sub-list values are left-justified and indented rather than right-justified, which reads better once wrapped. Hold-to-repeat scrolling added for both new screens via the shared menu auto-repeat (`pollSettingsKeys`). Version → 0.27.1.
- KeyReference (the key-reference sub-screen) still lacks hold-to-repeat scroll — same gap, but outside this change's scope. Candidate for a small follow-up.
- Feedback: `/` (right) now activates the selected row in the audit menu, matching action rows elsewhere in Settings (Enter still works too). Version → 0.27.2.
- Map catch-up (documentation impact): the shared menu-navigation idiom — `;`/`.` move, `,`/`/` adjust/activate, Enter activate, `Del`/`` ` `` back — is a consistency contract every settings menu (including this new one) must honour, but the map doesn't state it as a single rule. Note it in the map (e.g. the Settings or Controls and Navigation node) so future menus inherit it by design rather than by imitation.

## Conclusion

Reshaped on feedback from the plan's single flat list into a two-layer view (Stale / Current / Delete all stale, each count opening a scrollable key list). Sub-list values are left-justified rather than the originally-proposed right-justified — wrapping reads better that way.

Documentation impact (map catch-up, negotiated separately): the Persisted State node understates the chess namespace (6 keys, not 3); and the shared menu-navigation idiom should be recorded as an explicit consistency contract. Both noted in the Log.

### Proposed changelog entry

## 0.27.2 — 2026-06-05

- New Settings → "Settings data" screen for inspecting the saved-settings store (NVS). Shows how full the store is, counts of current versus stale keys (leftovers from older firmware), and a guarded "delete all stale" action. Each count opens a scrollable list of those keys and their values.
