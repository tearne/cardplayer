# Fn-strict keyboard handling

**Mode:** Formal

## Intent

Today the Fn modifier affects only two bindings; for every other key it's silently ignored — `Fn+-` does volume-down exactly as `-`. The user can't tell from how the device responds whether Fn matters.

Make Fn handling predictable: when Fn is held, only Fn-modified bindings fire; otherwise only plain. Fn either does something distinct or nothing — never the same as plain.

## Approach

### Gate at the top of the keypress handler

A top-level `if (state.fn)` / `else` split before any switch dispatch. Each branch handles only its own bindings; pressing Fn+key with no Fn binding is silently ignored, matching the existing no-op for any unmapped plain key.

### Modals stay permissive

The reset and help overlays still dismiss on any keypress, Fn-modified included. The gate doesn't apply to the modal-dismiss path — panic-cancel for a destructive prompt has to work no matter what the user pressed.

### Existing bindings move, behaviour preserved

`,` and `/` stay in the plain branch; `Fn+,` and `Fn+/` move into the Fn branch as plain `,` / `/` there. The inline `if (state.fn)` checks disappear; net user-visible behaviour is unchanged.

## Plan

- [x] Refactor `loop()`'s keypress dispatch into Fn-held / Fn-not-held branches; the existing inline `if (state.fn)` checks on `,` and `/` collapse into the branch structure. Modal-dismiss path stays permissive — any key dismisses regardless of Fn.
- [x] Bump `APP_VERSION` from `0.16.0` to `0.16.1`
- [x] On-device verification: `Fn+,` / `Fn+/` still skip tracks; plain `,` / `/` still navigate; pressing Fn alongside any other key (volume, font, diagnostics toggle, wrap, help, reset, etc.) does nothing; reset modal still opens on `Del` and dismisses on any key including Fn-modified ones; help overlay same.

## Log

- 2026-05-09: Plan covered the keypress switch in `loop()` but during implementation the same Fn gating turned out to apply to `pollSeekKeys` (`[`/`]` repeat-seek) and `pollBrowserNavKeys` (`;`/`.` repeat-scroll), which run on every loop iteration and read state directly. Added matching Fn early-return in both — kept the policy uniform across direct keypresses and held-key polling. Also added `g_show_reset_modal` to their early-return guards (previously only `g_show_help` was checked) since they shouldn't fire while a destructive prompt is up.

## Conclusion

Shipped. Fn modifier now means "alternate or nothing" everywhere keypresses are read — both the direct-press dispatch in `loop()` and the held-key polls (`pollSeekKeys`, `pollBrowserNavKeys`). The held-key gate wasn't in the Plan but the policy doesn't hold without it. Map's Controls node updated in-flight to document the modifier policy (tightly-bound, the prose was finalised before the build wrapped).

### Proposed `CHANGELOG.md` entry

```
## 0.16.1 — 2026-05-09

- Holding `Fn` no longer falls through to plain key actions on keys with no Fn binding. Track-skip `Fn+,` / `Fn+/` unchanged.
```
