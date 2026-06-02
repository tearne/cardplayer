# Drop the browser Del→reset binding

**Mode:** Wander

## Intent

In the plain browser, `Del` opens the destructive "Reset all" confirmation modal — a leftover from before the Ctrl-only keymap overhaul that no longer fits the model. Reset is a Settings action (Settings → Reset all); a stray `Del` springing a destructive prompt is a wart.

Drop the `else showResetModal()` arm of the browser `Del` handler. `Del` stays backspace in [Fuzzy Search] and cancel-back in track-pick mode; in the plain browser it becomes a no-op. Reset remains reachable via Settings → Reset all.

Map follow-up: the `Reset Confirmation` node currently notes it's reached "(and from `Del` in the browser)" — accurate today, so it stays until this lands, then the parenthetical is removed.

Blocked on the build lock: created while `alarm-tweaks.md` is the active change; pick up once that concludes.
