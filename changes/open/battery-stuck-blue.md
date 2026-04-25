# Battery stuck blue

## Intent

The battery indicator in the header is always shown blue (the 40–80% charge colour), regardless of the actual charge level. The exception is when the device is powered off and plugged in via USB, where the indicator behaves differently. The aim is for the indicator to reflect the real battery state at all times.

## Approach

### Add a charging indicator; leave the level estimate alone

The Cardputer ADV's AW32001 PMIC is a charge controller, not a fuel gauge. `M5.Power.getBatteryLevel()` is estimated from battery voltage, and voltage drops under load — so the estimate naturally caps below the >80% green threshold while running on battery. Calibrating the estimate is fragile (varies with load, temperature, battery age) and not worth chasing.

Instead, supplement the level with an explicit charging indicator driven by `M5.Power.isCharging()`, which is genuine hardware state from the PMIC. That gives the user a reliable "topping up" signal independent of the level estimate's accuracy.

### Map edits

**Updated node — Battery (charging indicator added; level caveat noted):**

```markdown
# Battery

[Up](#header)

A small icon at the right end of the header showing remaining charge and charging state. Icon only — no percentage text.

**Detail**

- Icon 27px wide × 8px tall, at the right end of the header.

- Level read via `M5.Power.getBatteryLevel()` (returns 0–100, negative if unavailable). The Cardputer ADV's PMIC is a charge controller, not a fuel gauge — the level is estimated from battery voltage, which drops under load, so the reading caps below the >80% green threshold while the device runs on battery.

- A small lightning bolt is drawn over the icon when `M5.Power.isCharging()` is true.

- Polled every few seconds; the reading changes slowly.

- Fill colour by level: `> 80%` green, `> 40%` blue, `> 20%` yellow, `> 10%` red, `≤ 10%` bright red.
```

## Unresolved

- Charging-state visual: a small lightning bolt drawn *over* the icon (centred, contrasting colour). Alternatives: drawn *beside* the icon (uses adjacent pixels but shrinks the bar), or a coloured outline around the whole icon. Overlay is the proposal in the node — confirm or pick differently. (Approach: "Add a charging indicator; leave the level estimate alone")

## Asides

- Review the community fork [WuSiYu/M5Cardputer-UserDemo-Plus](https://github.com/WuSiYu/M5Cardputer-UserDemo-Plus) before finalising — the official factory firmware turned out to be the most basic implementation possible, but the community fork may have added something worth borrowing.
