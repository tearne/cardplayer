# Reference notes — Cardputer ADV hardware

Working notes on hardware details that are non-obvious from the code, for use during planning. Datasheets and the schematic that back these notes are alongside this file.

- `Cardputer-ADV-schematic-v1.0.pdf` — official M5Stack schematic, fetched from [docs.m5stack.com](https://docs.m5stack.com/en/core/Cardputer-Adv).
- `TP4057-datasheet.pdf` — battery-charger IC datasheet, from DigiKey.


## Charging system

The charger IC is a **TP4057** (U1 on schematic page 1). Linear single-cell Li-ion charger in a small SMD package. **No digital interface** — no I2C, no SPI, no addressable registers. The chip exists purely as analog power-management hardware.

Visible TP4057 pins on the schematic:

- `VCC` ← `+5VIN` through R1 = 0.8 Ω (sense / inrush limit).
- `BAT` ↔ `VBAT_IN` net (battery side).
- `PROG` → R5 = 3.3 kΩ to GND. Charge current ≈ 1000 / R_prog mA = **~300 mA**.
- `CHRG`, `STDBY` — open-drain status outputs. Active low (pulled low when in the corresponding state). At schematic resolution these pins **do not appear to be routed to any Stamp-S3 GPIO**; they likely drive an indicator LED (D3 sits in the right area) or are unconnected. Worth re-verifying on the original schematic source or with a multimeter before assuming.
- Charge termination voltage is fixed by the IC at 4.2 V ±1 %.

**Implication:** there is no software-readable "is charging" signal on this board through any I2C register. The only candidates for software detection are (a) a GPIO routing of CHRG/STDBY we haven't found, or (b) heuristics from voltage behaviour (loaded vs unloaded sag, trend over time).


## Power architecture

USB-C → `+5VIN`, which fans out two ways:

1. **Charge path:** `+5VIN` → TP4057 → `VBAT_IN` (battery).
2. **System path:** `+5VIN` → Schottky D1 (SS34) → `VBAT_OUT` → SY7088 boost (`+5VOUT`) → SY8089 buck (`+3.3V`).

`Q1` (LP3218 P-FET) acts as a **battery-isolation MOSFET**: its source/drain sit between battery and `VBAT_OUT`. When USB is present, `+5VIN` raises Q1's gate and the FET turns off, isolating the battery from the system rail (system runs from USB through D1). When USB is absent, Q1 turns on and the battery feeds the system rail through Q1.

`SW1` is the user-facing power switch. It controls `Q2`/`Q3` (also LP3218 P-FETs) which gate `BAT+` onto `VBAT_IN`. With SW1 off, the battery is disconnected from the charge path and from the system. M5Stack's docs say "switch power to ON when charging" because of this — TP4057 sees no battery to charge with the switch off.

> Subtle consequence: with the device on and USB plugged in, the system runs from USB while the battery charges in parallel through the TP4057 at 300 mA. Loaded battery voltage doesn't move much during charging because the system isn't actually drawing from the battery — it's running off the USB rail. So a stable loaded voltage on USB is consistent with charging happening, and is *not* useful as a "not charging" signal.


## Battery voltage measurement

Battery voltage reaches the Stamp-S3 module on **GPIO 10** via a 2:1 divider (two ~100 kΩ resistors visible near the Q1 region of the schematic). The ADC must be read with `ratio = 2.0` to recover the cell voltage.

M5Unified handles this directly: `Power_Class.cpp:222-227` registers Cardputer ADV as `pmic_t::pmic_adc` with `_batAdcCh = ADC1_GPIO10_CHANNEL` and `_adc_ratio = 2.0f`. `M5.Power.getBatteryVoltage()` returns mV from this path with no I2C involvement.

The PMIC's own `getBatteryLevel()` (when used) maps the voltage onto a fixed 3.3–4.1 V range. This mapping doesn't match the cell's loaded behaviour on this device — see the `battery-stuck-blue` change for the calibrated two-point alternative.


## I2C bus inventory (`In_I2C`)

`In_I2C` runs on **GPIO 8 / GPIO 9**. The peripherals on this bus, from the schematic:

- **TCA8418** keyboard scanner @ `0x34` — INT on GPIO 11 (page 2 of schematic).
- **BMI270** 6-axis IMU @ `0x69` (page 3).
- **ES8311** audio codec @ `0x18` default (page 3) — its config-I2C pins share the bus.

**No charger IC** on this bus. Probing `0x49` returns NACK (chip ID register reads `0x00` via `M5.In_I2C.readRegister8`, which `.value_or(0)`'s on failure).


## M5Unified's view of this board

For Cardputer ADV, M5Unified does **not** know about any PMIC IC — it sets `_pmic = pmic_t::pmic_adc` and uses the GPIO 10 ADC for voltage. Consequently:

- `M5.Power.isCharging()` returns `is_charging_t::charge_unknown` (no code path produces a real answer for this board).
- `M5.Power.getBatteryLevel()` works but uses the generic 3.3–4.1 V mapping.
- `M5.Power.getBatteryVoltage()` works correctly via ADC.
- `M5.Power.setChargeCurrent()` / `setBatteryCharge()` / `setChargeVoltage()` have **no effect** — they fall through to the AW32001/AXP cases which aren't selected for `pmic_adc`.

The `AW32001_Class` driver inside M5Unified exists for a different board (`board_ArduinoNessoN1`, Power_Class.cpp:145). Seeing that driver in the library does **not** imply the chip is on Cardputer ADV.


## Pitfalls observed

> [!IMPORTANT]
> When a vendor library's code path "isn't compiled for your SoC", do not assume the chip is present. Trace the library's board-to-chip mapping for your specific board first. For M5Unified that's `Power_Class.cpp` setup (search for `board_M5CardputerADV`).

> [!IMPORTANT]
> Before issuing register reads against a presumed I2C device, perform the chip-ID handshake the vendor's reference driver performs in `begin()`. For AW32001-class chips that's a single byte read of the chip-ID register; for TP4057-class chips there is no I2C interface at all and no handshake to perform.

> [!IMPORTANT]
> `M5.In_I2C.readRegister8(...)` returns `.value_or(0)` on NACK. Always-zero reads are indistinguishable from "device says zero" without a chip-ID probe or by reading a register whose default value is non-zero.
