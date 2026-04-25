# Battery monitor

## Intent

Show a small battery indicator at the bottom of the display so the user knows roughly how much charge remains while using the device.

## Approach

First realisation of the **Header** region. Introduce a 10px strip at the top of the display and place a battery icon at its right end. Read charge via `M5.Power.getBatteryLevel()`, polled every few seconds. Icon only — no percentage text.

Fill colour reflects charge level:

- `> 80%` green
- `> 40%` blue
- `> 20%` yellow
- `> 10%` red
- `≤ 10%` bright red

The Footer is not introduced by this change; the existing bottom status line and progress bar stay as they are. The track list loses one row to make room for the header.

### Map edits

A new **Battery** node is added as a child of **Header**. This temporarily violates the only-child rule; accepted on the understanding a sibling (e.g. *Notifications*) will land shortly.

**New node — Battery (child of Header):**

```markdown
# Battery

[Up](#header)

A small icon at the right end of the header showing remaining charge. Icon only — no percentage text.

**Detail**

- Read via `M5.Power.getBatteryLevel()` (returns 0–100, negative if unavailable).

- Polled every few seconds; the reading changes slowly.

- Fill colour by level: `> 80%` green, `> 40%` blue, `> 20%` yellow, `> 10%` red, `≤ 10%` bright red.
```

**Updated node — Header (adds Down link, adjusts prose):**

```markdown
# Header

[Up](#screen-layout)
[Down](#battery)

A 10px strip at the top of the display carrying status indicators and transient notification banners. Status is always visible; notifications appear briefly in response to events (e.g. a volume change) then fade.
```

**Updated tree overview in the root:**

```
Application
├ Screen Layout
│ ├ Header
│ │ └ Battery
│ └ Footer
└ Controls
```

## Plan

- [x] Reserve a 10px header region at the top of the display; shift the track list down so it starts below the header and loses one row.

- [x] Add a function that returns the current battery level and a derived fill colour per the threshold table.

- [x] Draw the battery icon (outlined shape, fill proportional to level, colour by threshold) at the right end of the header.

- [x] Poll battery level every few seconds in the main loop and redraw the header when the level changes.

- [x] Add the **Battery** node to `map.md` as a child of Header.

- [x] Update the **Header** node in `map.md` — add `[Down](#battery)` link and mention the 10px height.

- [x] Update the root tree overview in `map.md` to include Battery under Header.

## Conclusion

Minor deviation: `STATUS_Y` was nudged from 117 to 118 (one pixel) so the track list's new bottom row doesn't touch the status line underneath. Not worth reflecting in the map.

Not yet verified on-device — the firmware compiles but I haven't flashed and watched the icon render.

