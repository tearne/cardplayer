# Chess difficulty hot-key

**Mode:** Wander

## Intent

Let the user change chess difficulty while inside chess, without having to exit to Settings. The current path — exit chess, open Settings, scroll to "Chess level", step the value, leave Settings, re-enter chess — is too many steps for what should be a casual mid-game adjustment (e.g. "this is too hard, drop to Medium and keep playing").

Likely shape: a single key (or modified key) pressed while chess is active that cycles Easy → Medium → Hard → Easy, with a short visual confirmation of the new level. The Settings row stays as the canonical place to see / set the value persistently — the hot-key just calls into the same `chess::setDifficulty()` so persistence and behaviour are identical. Specific key, cycle direction, and visual feedback to be settled in Approach.

## Conclusion

Shipped as 0.22.11.

**Decisions taken during build** (Wander has no Approach to record these up front):

- **Key: Tab**, unmodified. Free in the chess key map (the existing "any letter exits chess" rule doesn't catch Tab); single keystroke matches the "on the fly" feel.
- **Cycle: forward only**, Easy → Medium → Hard → Easy. One direction keeps the binding simple; users can wrap.
- **Feedback: permanent panel display** rather than the momentary flash the Intent hinted at. Added a `level` / value pair in the side panel under CHESS. Always visible is more useful than a flash — the user can check what they're up against at any moment, not just right after a change.
- **Persistence**: reuses `chess::setDifficulty()`, which already calls `save()`. Identical NVS path to the Settings row.
- **Discoverability**: footer hint expanded from `esc:exit` to `esc:exit tab:lvl` so the new binding shows up where users already look.

### Proposed changelog entry

```
## 0.22.11 — 2026-05-28

- Chess: Tab while in chess cycles CPU difficulty Easy → Medium → Hard → Easy without leaving the game. The side panel now shows the current level permanently. Persistence reuses the Settings row's path, so the value sticks across reboots and is reflected back in Settings → Chess level.
```
