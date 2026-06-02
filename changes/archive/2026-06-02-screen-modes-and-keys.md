# Screen Modes and Key Mappings

**Mode:** Explore

## Intent

The device has grown a lot of screen modes — browser, search, visualisation overlay, settings, key reference, chess, the reset modal, and now the alarm family (standby clock, Alarms, alarm editor, Days, set-time, track-pick). Each was added in its own change, and the key mappings and the way modes nest and hand off to each other have never been reviewed as a whole.

Step back and review the full picture: which modes exist, how they relate, how a user enters and leaves each, and which keys do what in each. The goal is a coherent, documented model — surfacing inconsistencies, overloaded keys, and confusing transitions — and bringing the map up to date (including the `Alarm (TODO)` placeholder) as part of the same effort.

## Approach

### A screen tree with one back-out rule

Model every screen as a node with exactly one parent. Esc (and its equivalents) always backs out to the parent, regardless of how the screen was entered — so the visualiser, though toggled with Tab, is a child of the main screen and Esc backs out of it. *Reason: one uniform back-out rule replaces the per-screen ad-hoc exit handling that keeps drifting (e.g. the recent clock-vs-back-out regression).*

### Diagnostics is a main-screen overlay, not a node

A toggleable attribute of the main screen (browser + visualiser), invisible elsewhere and outside the back-out tree. *Reason: it's a property of one screen, not a destination.*

### One canonical screen state

The model names a single current screen (its position in the tree) rather than today's scattered `g_show_*` flags and long dispatch chain. *Reason: those scattered flags are where transitions and Esc handling drift apart.*

### Modifier consolidation: Ctrl + unshifted base keys only

Commands use only Ctrl and unshifted base keys (letters, digits, Tab, Enter, Del, Space, `;` `.` `,` `/` `` ` ``). Fn is dropped entirely, and Shift is retained *only* for typing text (search query, chess notation), never as a command modifier. Two code constraints shape the remap: several commands are today symbols the keyboard only produces with Shift (`?`, `{`, `}`, `~`, `+`, `_`), so each needs a new home on a base key or Ctrl-combo; and holding Ctrl makes the keyboard emit the *shifted* character (`Ctrl+w` → `W`), which the scheme must design around. The review determines whether every command can be rehoused within this constraint. *Reason: Ctrl is the easiest modifier to hit on this keyboard; one modifier plus base keys is the simplest thing to learn and to reach one-handed.*

### Deliverable is the design, not the implementation

This change produces the documented model only — the screen tree, the Esc/back-out rules, the diagnostics scoping, a concrete Ctrl-only keymap proposal with a feasibility verdict, and the map catch-up. The dispatch refactor and the key remap land as separate follow-on changes. *Reason: the refactor and remap are large and risky; settling the model first lets them proceed against an agreed target.*

## Plan

**Topics**

- **Screen tree** — every screen as a node with exactly one parent and a uniform Esc/back-out rule; flag where today's behaviour breaks it.
- **Overlay classification** — place diagnostics, the visualiser, and the modals (reset, key reference) as overlays vs tree nodes relative to the main screen.
- **Ctrl-only keymap** — survey current bindings; remap every command onto Ctrl + unshifted base keys; rehome the Shift-only symbols (`?` `{` `}` `~` `+` `_`); resolve the Ctrl→shifted-char quirk; verdict on whether full Ctrl-only is achievable.
- **Inconsistencies** — catalogue overloaded keys, ad-hoc/conflicting transitions, and back-out violations surfaced by the review.
- **Map catch-up** — fill in the `Alarm` node and represent the screen-tree / keymap model in the map (per-node negotiation).

**Done when** the change document holds the screen tree with its back-out rules, the overlay classification, a Ctrl-only keymap proposal with a feasibility verdict, and a list of inconsistencies to fix; and the map reflects the model. Implementation (dispatch refactor, key remap) is left to follow-on changes.

## Conclusion

Delivered a design, not code: the screen tree with a single `` ` `` back-out rule (Standby at the root), the diagnostics/overlay classification, a five-way key-category model with a global-transport pass-through rule, and a Ctrl-only keymap — verdict: achievable, with Fn and all Shift-commands retired and Shift left only for typing.

Deviations: map catch-up deferred to the implementation change (a design-only change shouldn't map proposed state); the `Alarm (TODO)` placeholder stands. Doc fix: CHANGELOG 0.23.12 corrected (`Ctrl+A` → `` ` ``); the matching stale comment at `main.cpp:4610` is left for the implementation.

Follow-on: an implementation change for the dispatch refactor + key remap.

## Findings

### Screen inventory

Sixteen distinct UI states exist today. They sort into four kinds:

- **Main-area contents** (header + footer stay; only the middle changes): **Browser**, **Search**, **Visualisation** (waveform / spectrum / test). Mutually exclusive contents of one main screen.
- **Full-screen overlays** (replace everything, navigable hierarchy): **Settings → { Key Reference, Reset modal, Alarms → { Set Current Time, Alarm Editor → { Days, Track Picker } } }**.
- **Full-screen siblings of the main screen**: **Chess**, **Standby Clock**.
- **System interrupt** (pre-empts *any* state, restores it on dismiss): **Alarm Firing → Snoozing**. Not reached by navigation, so not a tree node.

Two cross-cuts worth noting: the **Track Picker** is the *Browser reused* in "pick mode" (yellow theme, `g_pick_slot >= 0`), launched from the Alarm Editor and returning to it — a navigational leaf implemented by borrowing a main-area content. **Diagnostics** is not a state at all; it's a header overlay toggled on/off.

### Proposed screen tree (with the one back-out rule)

```
Main  (Browser | Search | Visualisation)      ← Esc cycles content back to Browser
├ Settings
│ ├ Key Reference
│ ├ Reset (modal)
│ └ Alarms
│   ├ Set Current Time
│   └ Alarm Editor
│     ├ Days
│     └ Track Picker        (borrows Browser; returns here)
├ Chess
└ Standby Clock

Alarm Firing → Snoozing       (interrupt; dismiss restores the pre-alarm state)
Diagnostics                   (header overlay on Main only; not in the tree)
```

**Back-out rule:** one key (Esc) always moves to the parent; at the Main root it has nothing to back out to. Search and Visualisation are *contents* of Main, so Esc returns them to the Browser content rather than leaving Main.

### Overlay classification

- **Diagnostics** — a toggleable header overlay, visible only on Main (Browser + Visualisation), outside the back-out tree. Confirmed as the user's model.
- **Visualisation** — a Main-area content (a screen you Esc out of), *not* a separate overlay; Tab additionally toggles/restores it. Today it is the one place where Esc/`` ` `` does **not** back out — it enters Standby instead (the bug class this change targets).
- **Reset modal / Key Reference** — true children of Settings.

### Back-out (Esc) — current inconsistencies

The "back" gesture is implemented per-branch and is not uniform:

1. **No single Esc key.** Back-out is variously `` ` `` (browser/pick/search), `Fn+\`` (every editor + settings + alarm), `Del` (settings, alarms, editors, reset, pick), or "any key" (Help, Chess, Standby). Five different ways to mean "back".
2. **`` ` `` is overloaded** — it backs out of pick/search, but at the browser root and *inside Visualisation* it enters the Standby clock instead of backing out. The visualiser therefore can't be Esc'd back to the browser.
3. **"Any key dismisses"** (Help, Chess, Standby) vs an explicit back key elsewhere — inconsistent and easy to trigger by accident.
4. **Settings consumes letters as dismiss** while the Browser uses letters to start Search — the same key means opposite things one level apart.

### Key mapping — current modifier load

- **Fn** carries only: `Fn+\`` (Esc/back) and `Fn +/-` (brightness). Nothing else.
- **Shift** is never a command modifier; instead six commands *are* shifted symbols the keyboard only makes with Shift: `?` (Settings), `{` `}` (skip ∓), `~` (rebuild index), `+` `_` (font ∓).
- **Ctrl** carries `Ctrl+W/S/T/H/D` (waveform / spectrum / test / chess / diagnostics).
- **Library quirk:** holding Ctrl (or Shift) makes the keyboard emit the *shifted* character — `Ctrl+w` → `W`, and `Ctrl+1` → `!`, `Ctrl+/` → `?`. So Ctrl combos with letters are clean (uppercase), but Ctrl combos with digits/punctuation produce symbols.

### Ctrl-only keymap proposal

**Why it's feasible:** in the Browser, plain letters already open Search, so command chords can't use bare letters anyway — they must be modified. Moving every chord-command to **Ctrl+letter** is clean: there are 26 letters and only ~12 commands, and the Ctrl→shifted quirk is harmless for letters (just match uppercase). The base navigation/transport keys are already unshifted and keep their roles.

Proposed scheme:

| Stays on base keys (unshifted) | Role |
|---|---|
| `;` `.` | move cursor |
| `,` | ascend / parent |
| `/` | activate / enter / play |
| `space` | pause |
| `=` `-` | volume ∓ |
| `[` `]` | tap = skip prev / next; hold = seek (scrub) |
| `'` | jump to playing |
| `\` | wrap names |
| `0–9` | jump to tenth |
| letters | Search text |
| `` ` `` | **Back / Esc (universal); at the root → Standby** |

| Moves to Ctrl combos (letter mnemonics provisional) | From |
|---|---|
| Brightness ∓ (context-sensitive) — `Ctrl+-` / `Ctrl+=` | `Fn +/-` |
| Settings — `Ctrl+E` | `?` |
| Rebuild index — `Ctrl+R` | `~` |
| Waveform / Spectrum / Test — `Ctrl+W/S/T` | (unchanged) |
| Chess — `Ctrl+H` | (unchanged) |
| Diagnostics — `Ctrl+D` | (unchanged) |

Brightness pairs with volume as a mnemonic: `-`/`=` = volume, `Ctrl+-`/`Ctrl+=` = brightness. It is **context-sensitive** — adjusts the standby/clock brightness while the full-screen clock is up, the normal screen brightness otherwise. *Implementation note:* under Ctrl the keyboard emits the shifted char, so `Ctrl+-` arrives as `_` and `Ctrl+=` as `+`; the handler matches those (as today's `+`/`_` font shortcut already does).

Skip and seek share the `[`/`]` keys: a **tap** skips track (prev/next), a **hold** scrubs (the existing seek). This drops the shifted `{`/`}` skip pair and keeps both functions on the natural prev/next keys. *Implementation note:* the on-press fires skip only if the key is released before the seek hold-threshold; `pollSeekKeys` already owns the hold path.

| Dropped (redundant) | Rationale |
|---|---|
| Font ∓ (`+`/`_`) | already a Settings row; rarely used |
| `Fn+\`` Esc | replaced by base `` ` `` = Back |

**Key move:** make `` ` `` the universal **Back/Esc** (it sits where Esc lives, needs no modifier). Backing out past the root — i.e. pressing it on the plain Browser, where nothing is above — enters the **Standby clock**. This is *not* an overload: standby is simply "back out from the top". It fixes the visualiser bug (inside Visualisation, `` ` `` backs out to the Browser; pressing it again at the root enters Standby) and gives every screen one consistent back key — no Fn, no Shift-symbols, no separate standby chord.

The `Ctrl+letter` mnemonics above are a starting proposal, not settled — they're easy to tune when the implementation change picks them.

**Verdict: full Ctrl-only is achievable.** Every command lands on either an already-unshifted base key or a `Ctrl+letter`; Fn and all Shift-dependent command symbols are eliminated; Shift is left purely for typing search text and chess notation. The only judgement calls are the dropped redundancies (font/brightness shortcuts) and the specific letter mnemonics, both easy to tune.

### Key categories — where each applies

Every key falls into one category, and "does it work on other screens" is decided by the category, not per-screen:

1. **Back** — `` ` `` — works on *every* screen (the universal back-out / Standby-at-root).
2. **Global transport & display** — pause (`space`), skip/seek (`[`/`]`), volume (`-`/`=`), brightness (`Ctrl+-`/`Ctrl+=`) — control the device's ongoing playback and screen regardless of what you're looking at, so they stay live on every screen **except** where that physical key is claimed for **text entry** (Search query, Chess notation) or a **modal decision** (Reset confirmation).
3. **Navigation / activation** — `;` `.` `,` `/`, Enter, Del — screen-local; meaning depends on the current screen.
4. **Text entry** — letters / digits / `space` — only in text-input contexts (Search, Chess).
5. **Mode switches** — `Ctrl+W/S/T/H/E/R`, Standby — entered from the **Main screen** only (back out to Main first); not global.

**Why this is now clean:** the Ctrl-only scheme makes category 2 conflict-free on the menu/editor screens — those use `;`/`.`/`,`/`/` for navigation, leaving `space`, `-`/`=`, `[`/`]`, `Ctrl+-`/`=` free to pass straight through. The rule only has to *yield* in two well-defined places (text entry, modal).

Two behaviour implications to confirm:
- **Skip/seek (`[`/`]`) would now pass through on Settings/Alarms/Editors too** (today only pause + volume do) — making the whole transport set uniform.
- **Chess:** transport keys should pass through *without exiting chess* (music control while playing), rather than today's "any unrecognised key exits". Chess still consumes its own game keys; only the global-transport set is let through.

### Inconsistencies catalogue (for the follow-on implementation)

1. Five different "back" gestures; no single Esc.
2. `` ` `` overloaded (back vs enter-standby), blocking Esc-out of the visualiser.
3. "Any key dismisses" in Help / Chess / Standby.
4. Letters mean Search in Browser but dismiss in Settings.
5. Six commands depend on Shift-only symbols (`? { } ~ + _`).
6. Brightness/font exist both as shortcuts and as Settings rows (duplication).
7. **Docs vs code drift:** Standby is `` ` `` in code, but the alarm Approach and a stale comment at `src/main.cpp:4610` say `Ctrl+A`. (Decided: `` ` `` is correct, `Ctrl+A` dropped. Changelog corrected; the stale code comment is left for the implementation change.)
8. Auto-repeat is handled in three separate pollers (`pollSettingsKeys`, `pollBrowserNavKeys`, `pollVolumeKeys`) plus `pollSeekKeys` — scattered, mode-gated by hand.
9. Transport pass-through is uneven: pause + volume reach Settings/Alarms/Editors but skip does not; modals/Chess swallow or exit on them. No defined "global" key category (resolved by the Key categories rule above).

## Log

- Inventory gathered via two read-only sub-agents (screen modes/transitions; key bindings/modifiers). No surprises in the mode list.
- Surprise: Standby is bound to `` ` ``, not `Ctrl+A` as the alarm change's Approach/changelog and a stale code comment (`main.cpp:4610`) claim. Recorded as inconsistency #7.
- Decisions: standby stays on `` ` `` (Esc) at the main root — `Ctrl+K` proposal dropped; `Ctrl+A` retired. Changelog `0.23.12` corrected (`Ctrl+A` → Esc/`` ` ``). Map catch-up deferred to the implementation change (design-only change shouldn't map proposed state); `Alarm (TODO)` placeholder left as-is. Ctrl+letter mnemonics left provisional.
- Brightness kept (not dropped): `Ctrl+-`/`Ctrl+=`, context-sensitive (standby vs normal), pairing with `-`/`=` volume. Only the font ∓ shortcut is dropped (stays in Settings).
- Skip prev/next moved off `Ctrl+P`/`Ctrl+N` onto the base `[`/`]` keys: tap = skip, hold = seek; shifted `{`/`}` dropped.
- Added a key-category model (user prompt: no explicit rule existed for transport pass-through). Category 2 "global transport & display" stays live on every screen except text-entry / modal contexts. Implies skip/seek pass through menus too, and Chess lets transport through without exiting — both flagged for confirmation.
