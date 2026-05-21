# Drop the enter key entirely

**Mode:** Formal

## Intent

Two issues:

1. **Bug:** `/` in the search overlay activates the directory-cursor's entry instead of the highlighted search result. The dispatch routes `/` to `activateSelection()` unconditionally, ignoring `g_search_active`.
2. **Inconsistency:** the prior change made `/` the universal browser activate; `enter` still lives on in search-activate, Settings actions, the reset modal, and the viz overlay dismiss list. Removing `enter` entirely makes `/` the single activate key everywhere.

## Approach

### `/` in search activates the search result

In the browser plain-keys dispatch, the `/` case becomes: if `g_search_active`, call `searchActivate()`; otherwise call `activateSelection()`.

### Settings actions activate on `/`

`adjustSettingsRow(+1)` currently no-ops on action rows (only handles toggles + numerics). Extend it so a `/` on an action row calls `activateSettingsRow()`. The Settings dispatch loses its `state.enter` branch entirely.

### Reset modal confirms on `/`

`/` confirms; any other key still dismisses. Replaces the previous `enter` confirm.

### Drop `enter` from help / viz dismiss

The help screen dismiss-on-any-key path drops the `|| state.enter` part. The viz overlay's dismiss-keys list (which currently includes enter, del, tab) keeps del and tab but drops enter.

## Plan

- [x] Browser dispatch: `/` calls `searchActivate()` when search is active, `activateSelection()` otherwise.
- [x] `adjustSettingsRow(+1)` activates action rows.
- [x] Settings dispatch: remove the `state.enter` activate branch.
- [x] Reset modal: confirm on `/`, no longer on `enter`.
- [x] Help screen: remove `|| state.enter` from the dismiss-on-any-key condition.
- [x] Viz overlay: remove `state.enter` from dismiss-keys.
- [x] Remove the standalone `if (state.enter && g_search_active)` block in the browser dispatch.
- [x] Smoke test all paths.
- [ ] Update map Controls node again — the `enter` bullet can be removed entirely.

## Conclusion

`enter` no longer has any role in the application. `/` is the single activate gesture everywhere: descend dirs, start audio entries, activate search results, activate Settings actions, confirm the reset modal. The search-overlay bug (where `/` activated the directory-cursor entry instead of the highlighted search result) is fixed as part of the consolidation. Settings toggle and numeric rows are unchanged; action rows now activate on `/` rather than `enter`.

**Documentation impact:** the map `Controls` node still has the `enter` bullet from the previous round of edits — needs to be removed. Per-node negotiation pending.
