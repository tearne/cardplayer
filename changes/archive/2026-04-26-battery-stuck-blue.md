# Battery stuck blue

## Intent

The battery indicator in the header is always shown blue (the 40–80% charge colour), regardless of the actual charge level. The exception is when the device is powered off and plugged in via USB, where the indicator behaves differently. The aim is for the indicator to reflect the real battery state at all times.

## Approach

### Map the bar from battery voltage with two-point calibration

The PMIC is a charge controller, not a fuel gauge — `M5.Power.getBatteryLevel()` is voltage-derived but uses a fixed 3.3–4.1V mapping that doesn't match this device under load. Bypass it: read `M5.Power.getBatteryVoltage()` directly and apply our own mapping with two configurable thresholds:

- **`LOADED_EMPTY_MV` = 3400** — the empty point. Conservative for cell longevity (well above the 2.5V damage threshold and 3.0V "discharged" point) and accounts for voltage sag under load.
- **`LOADED_FULL_MV`** — the full point. The voltage observed when the battery is fully charged and the device is running. TBD empirically.

Displayed level = `clamp((mv - LOADED_EMPTY_MV) * 100 / (LOADED_FULL_MV - LOADED_EMPTY_MV), 0, 100)`. Existing colour thresholds and bar-fill formula operate on this rescaled value. Bar becomes green when truly full, reads 0 when the cell is genuinely at the conservative-cutoff voltage.

### Auto-shutdown at 0%

When the displayed level hits 0 (voltage ≤ `LOADED_EMPTY_MV`), enter a 10-second warning state:

1. Stop playback cleanly.
2. Blank the screen, then draw a centred two-element warning: title "Battery Empty" in red and body "Charge with power switch ON" wrapped to two lines. All rendered at twice the default font size.
3. After 10 seconds, call `M5.Power.powerOff()` — ESP32 deep sleep with no wake sources configured (deepest the SoC offers, ~10 µA standby).

If at boot voltage is already at or below `CRITICAL_EMPTY_MV` (3300 mV), skip the warning and sleep immediately — the cell has dropped past the warning band during a previous off-then-on cycle without charging, and further on-time would only stress it.

The countdown is one-way. The Cardputer ADV's TP4057 charger has no software-readable charging signal (see `reference/notes.md`), so plugging in USB during the warning window has no effect on the countdown. Recovery: leave SW1 on (charging only happens with the power switch on, per M5Stack's docs and the schematic) and USB plugged in; while the ESP32 is in deep sleep there's no system load and the cell tops up at near the TP4057's full ~300 mA. To resume use, press the boot button (BTN1) or briefly unplug-and-replug USB to trigger a hardware reset.

### Add battery voltage to the diagnostics row

The header's existing diagnostics row carries `stk`, `buf`, `u`. Add a fourth field `bat:NNNN` showing battery voltage in mV. Useful permanently as ongoing visibility (matches the `format:` Serial line for audio formats), and serves the immediate calibration need: leave the device plugged in and running for an hour or two and read off the value the voltage stabilises at — that's `LOADED_FULL_MV`.

### Battery dev mode

A `BATTERY_DEV_MODE` constant at the top of the file shifts both empty thresholds upward (`LOADED_EMPTY_MV` 3700, `CRITICAL_EMPTY_MV` 3600) so the empty path can be exercised without depleting the cell. Production values are 3400 and 3300. The flag was set `true` during initial verification and is now `false`. It remains in place as the seed for the broader developer-mode work tracked in `dev-mode.md`.

### Map edits

Three nodes touched: tree overview at the **Application** root, an updated **Battery** node, and a new **Emergency Shutdown** node sitting under it.

**Tree overview (in the Application root node):**

```
Application
├ Screen Layout
│ ├ Header
│ │ ├ Version
│ │ ├ Battery
│ │ │ └ Emergency Shutdown
│ │ └ Diagnostics
│ ├ Browser
│ └ Footer
├ Playback
└ Controls
```

**Updated node — Battery (voltage-driven mapping; UI detail; no charging indicator):**

```markdown
# Battery

[Up](#header)
[Down](#emergency-shutdown)

A small icon at the right end of the header showing remaining charge, with the cell voltage as text directly below.

> [!IMPORTANT]
> No software-readable charging state on this hardware. The TP4057 charger is purely analog; its status pins are not routed to a GPIO. See `reference/notes.md`.

**Detail**

- Icon 30 px wide × 8 px tall. Voltage label `N.NNv` (2 dp) centred horizontally beneath the icon, in the diagnostics row.

- Level rescaled from voltage: `clamp((mv − 3400) × 100 / (3800 − 3400), 0, 100)`. Bounds (`LOADED_EMPTY_MV`, `LOADED_FULL_MV`) calibrated to the loaded voltage range on this hardware — the M5 default 3.3–4.1 V map gives a stuck-blue ceiling because a charged cell sags below 4.1 V under load. Empty is set above the 3.0 V damage threshold for cell longevity.

- Fill colour by level: green / blue / yellow / red / bright-red bands. Polled every few seconds.
```

**New node — Emergency Shutdown (under Battery):**

```markdown
# Emergency Shutdown

[Up](#battery)

When the cell hits the empty cutoff, the device protects itself by powering off cleanly rather than letting the cell discharge further.

**Detail**

- Triggered when displayed level reaches 0 (voltage ≤ `LOADED_EMPTY_MV`).

- Playback stops, the screen blanks, and a centred warning shows for 10 seconds. Then `M5.Power.powerOff()` puts the ESP32 into deep sleep.

- Warning text: title "Battery Empty" in red, body "Charge with power switch ON" wrapped to two lines. All at double font size.

- One-way: with no charging signal available, plugging in USB during the warning window has no effect. Recovery is a physical power-cycle.

- On boot, if voltage is already at or below `CRITICAL_EMPTY_MV` (~3300 mV), skip the warning and sleep immediately — the cell has dropped past the warning band during a previous off-then-on cycle without charging, and further on-time would only stress it.

- Recovery: leave SW1 on, plug in USB. While in deep sleep the ESP32 draws ~10 µA, so TP4057 charges the cell at near-full rate. To resume use, press the boot button (BTN1) or briefly unplug-and-replug USB to trigger a hardware reset.
```

## Plan

- [x] Add voltage-based level computation: `LOADED_EMPTY_MV` (3400) and `LOADED_FULL_MV` (initial 3850, to refine empirically via the new diagnostic) constants and a `displayedBatteryLevel()` helper that reads `M5.Power.getBatteryVoltage()` and rescales clamped 0..100. Replace the `getBatteryLevel()` call in `pollBattery()` with this helper.

- [x] Draw a small lightning bolt overlay on the battery icon in `drawHeader()` when `M5.Power.isCharging()` reports charging.

- [x] Add a `bat:NNNN` mV field to the diagnostics row in `drawDiagnosticsRow()`, refreshed on the same cadence as the existing fields.

- [x] Add a battery-low handler: when `pollBattery()` observes a level of 0, stop playback, blank the screen, draw the "Battery empty. Charge now to prevent damage. Powering off." warning centred. Poll `isCharging()` once per second for 60s — on charge, restore the normal UI and resume; on timeout, call `M5.Power.powerOff()`.

- [x] Update the **Battery** node in `map.md` per the Approach.

### Cleanup (post-investigation)

- [x] Remove the temporary `HH/HH` chip-id/SYS_STA diagnostic from `drawDiagnosticsRow()` and the supporting globals `g_battery_chg_raw` and `g_battery_chip_id`.

- [x] Remove `isChargingAw32001()` and the `g_battery_charging` global. Remove the purple-fill branch in `drawHeader()`; the icon's fill always uses the level-derived `batteryColour()`.

- [x] In the battery-low warning handler: remove the cancel-on-charge logic, reduce the countdown from 60s to 10s, replace the warning text with `"Battery empty. Powering off. To charge: plug in USB, switch power ON."`, and render the message at `setTextSize(2)` (or equivalent).

- [x] Add a `CRITICAL_EMPTY_MV` constant. On the first `pollBattery()` after boot, if voltage ≤ `CRITICAL_EMPTY_MV`, call `M5.Power.powerOff()` directly without entering the warning state.

- [x] Add a `BATTERY_DEV_MODE` constant. When `true`, set `LOADED_EMPTY_MV` = 3700 and `CRITICAL_EMPTY_MV` = 3600; when `false`, use 3400 and 3300. Leave the flag `true` for now.

- [x] Update the tree overview in `map.md`'s root **Application** node to add **Emergency Shutdown** as a child of **Battery**.

- [x] Update the **Battery** node in `map.md` per the revised Approach (parent of Emergency Shutdown; charging-state callout; trimmed Detail).

- [x] Add the new **Emergency Shutdown** node in `map.md` per the Approach (immediately after the Battery node in file order).

### Post-verification adjustments

- [x] Revise the warning text to title "Battery Empty" (red) + body "Charge with power switch ON" (wrapped to two lines).

- [x] Set `BATTERY_DEV_MODE` to `false` (production thresholds: `LOADED_EMPTY_MV` 3400, `CRITICAL_EMPTY_MV` 3300).

- [x] Update the **Emergency Shutdown** node in `map.md` to include the literal warning text.

## Log

- On-device observation: `LOADED_FULL_MV` set to 3830 (the voltage stabilised at after extended plug-in-and-running). Initial guess of 3850 was close.

- Charging bolt was initially drawn as a thin 3-line zigzag in white, then redrawn as a chunkier 4×5 yellow zigzag — still hard to spot at 8 px icon height. Abandoned the bolt approach entirely; charging is now indicated by changing the icon's fill colour to purple, which is unmistakable at any level.

- Battery icon widened from 27 to 30 px so its width matches the voltage text centred below it. Visual coherence — the icon and voltage now line up as one column.

- Battery voltage diagnostic reformatted to `N.NNv` (no prefix) and centred horizontally under the battery icon — the icon itself supplies the "this is battery voltage" context.

- `M5.Power.isCharging()` always returned `charge_unknown` on the Cardputer ADV — the AW32001 case in M5Unified's `Power_Class::isCharging()` is wrapped in `#elif CONFIG_IDF_TARGET_ESP32C6`, so it isn't compiled into the ESP32-S3 build for our hardware. (M5Unified treats the Cardputer ADV as `pmic_adc` for voltage reading, which works, but loses the charging-state path entirely.) Worked around by reading the AW32001's SYS_STA register over I2C ourselves — `M5.In_I2C.readRegister8(0x49, 0x08, ...)`, mask bits 4:3, treat pre-charge or charge as charging.

- Post-build verification: user reports never observing the purple charging-fill colour, and voltage stuck at 3.68v while running on USB but jumping to 4.09v after the device is switched off and plugged in. M5Unified configures the AW32001 with `setChargeCurrent(100)` and `setChargeVoltage(4200)` but does not raise the input current limit (IINLIM) above the chip's reset default. Likely cause: under system load the input limit is exceeded, the chip enters supplement mode, and SYS_STA bits 4:3 read `00` (not charging). Added a temporary on-screen diagnostic `c:HH` showing the raw SYS_STA byte to confirm, before deciding whether to replan and add an IINLIM fix.

- SYS_STA reads `0x00` in every state (USB on/off, device on/off). Reviewing M5Unified's `Power_Class.cpp:222`: Cardputer ADV is registered as `pmic_adc` (ADC voltage on GPIO10) — the AW32001 init we'd referenced (`Aw32001.begin`, `setBatteryCharge`, `setChargeCurrent`, `setChargeVoltage`) is gated to a different board (`ArduinoNessoN1`, line 145) and is never run on this hardware. M5Unified does not assert the AW32001 is present at `0x49` on `In_I2C` for Cardputer ADV — that was an assumption. `M5.In_I2C.readRegister8` returns `0` on NACK (`.value_or(0)`), so always-zero is consistent with no device at that address. Extended the diagnostic to also read the chip-ID register (`0x0A`, expected `0x49`) and display it as `HH/HH` (chip-id / SYS_STA); a chip-id of `00` will confirm the chip is not at `0x49`.

- Diagnostic confirmed: chip-id reads `0x00` in every state — no device responding at `0x49` on `In_I2C`. The whole register-read approach is wrong for this hardware. The Approach's premise that `M5.Power.isCharging()` (or a direct AW32001 read) would yield genuine charging state is false on Cardputer ADV: M5Unified knows the audio codec on `In_I2C` but no charger IC, and battery voltage on this board is read via ADC, not I2C. Our `isChargingAw32001()` has been silently returning false since the build was completed — the charging-fill colour was unreachable. **Blocker: no known way to obtain hardware charging state on this board without further reconnaissance** (I2C scan to find any responding charger IC, or consulting the Cardputer ADV schematic for a CHG_STAT GPIO line). Path forward is a planning question, not a build decision.

- Schematic-confirmed: charger IC is **TP4057** (linear single-cell Li-ion charger, no I2C, charge current ~300 mA via R5 = 3.3 kΩ on PROG). Its `CHRG`/`STDBY` open-drain status pins do not appear to be routed to any Stamp-S3 GPIO at the schematic resolution available — most likely they drive an LED or are unconnected. Schematic and TP4057 datasheet saved under `reference/`. With a 100 W USB supply the cell does charge while the device runs, just imperceptibly through the 3.6–3.8 V Li-ion plateau (60 mV in 20 minutes is consistent with full ~300 mA charge into 1750 mAh). The original "stuck" symptom on a weaker source was likely real USB current limiting, not a hardware fault. Closing out by dropping the charging indicator entirely; auto-shutdown becomes a one-way 10 s countdown.

- Post-verification (2026-04-26): warning text trimmed to two elements — "Battery Empty" title in red plus "Charge with power switch ON" body wrapped over two lines, all at double font size. Original five-line text was busy at that size. `BATTERY_DEV_MODE` flipped to `false` (production thresholds restored: `LOADED_EMPTY_MV` 3400, `CRITICAL_EMPTY_MV` 3300) — verification of the warning + countdown + immediate-sleep paths was sufficient. Map node updated to include the literal warning text rather than a generic reference.

## Conclusion

Shipped as a replan: the original charging indicator turned out to be unsupportable on this hardware (the Cardputer ADV's TP4057 charger has no software-readable status). The change was scoped down to voltage-driven level calibration plus an Emergency Shutdown failsafe.

Documents added outside the Plan:

- `reference/Cardputer-ADV-schematic-v1.0.pdf` — official schematic.
- `reference/TP4057-datasheet.pdf` — charger IC datasheet.
- `reference/notes.md` — hardware observations and pitfalls (TP4057 specifics, M5Unified's Cardputer ADV mapping, the wrong-chip-inference trap that bit us mid-build).
- `changes/open/dev-mode.md` — new exploratory proposal spawned as an aside, with `BATTERY_DEV_MODE` as its seed.

### Changelog entry

```
## 0.6.0 — 2026-04-26

- Battery indicator now reflects the real charge level — calibrated to the loaded voltage range seen on this device, so a charged cell shows green rather than sticking in the blue 40–80% band. Cell voltage is also shown numerically in the diagnostics row.
- New emergency shutdown when the cell hits empty: playback stops, a "Battery Empty" warning shows for 10 seconds, and the device enters deep sleep to protect the cell. Resume by plugging in USB with the power switch on, then power-cycling.
```

Bump `APP_VERSION` from `0.5.0` to `0.6.0` alongside the changelog entry.
