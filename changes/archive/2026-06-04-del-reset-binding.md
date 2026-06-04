# Drop the browser Del→reset binding

**Mode:** Wander

## Intent

In the plain browser, `Del` opens the destructive "Reset all" confirmation modal — a leftover from before the Ctrl-only keymap overhaul that no longer fits the model. Reset is a Settings action (Settings → Reset all); a stray `Del` springing a destructive prompt is a wart.

Drop the `else showResetModal()` arm of the browser `Del` handler. `Del` stays backspace in [Fuzzy Search] and cancel-back in track-pick mode; in the plain browser it becomes a no-op. Reset remains reachable via Settings → Reset all.

Map follow-up: the `Reset Confirmation` node currently notes it's reached "(and from `Del` in the browser)" — accurate today, so it stays until this lands, then the parenthetical is removed.

Blocked on the build lock: created while `alarm-tweaks.md` is the active change; pick up once that concludes.

## Log

- Handler found unchanged at the browser `Del` branch; dropped the `else showResetModal()` arm and reworded the comment. `showResetModal` stays live (Settings → Reset), so no dead code.
- Built clean at 0.26.29; awaiting hardware check (Del in the plain browser does nothing; Settings → Reset all still works).
- Map follow-up done: removed "(and from `Del` in the browser)" from the `Reset Confirmation` node, leaving Settings → Reset all as its only entry point.

## Conclusion

Completed as intended — the browser `Del` no-ops in the plain listing, reset stays a Settings action. The handler was unchanged from the Intent's description (unlike the previous two changes), so no divergence. Map's `Reset Confirmation` node caught up in the same pass.

Shipped at **0.26.29**.
