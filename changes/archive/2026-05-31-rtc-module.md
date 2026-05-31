# RTC Module

**Mode:** Wander

## Intent

An M5Stack RTC module is connected via Port A (Grove). Confirm it is detected and readable, then verify it can supply the player with a consistent wall-clock time across reboots. Once confirmed, this becomes the foundation for an alarm-clock feature to follow as a separate change.

## Log

- HYM8563 is PCF8563-compatible; M5Unified's `M5.Rtc` already drives PCF8563 and auto-binds to Port A via `cfg.external_rtc = true`. No third-party driver needed.
- Cardputer ADV Port A pins per M5Unified's board table: SCL=GPIO1, SDA=GPIO2 (Ex_I2C).
- 0.22.12: enabled `cfg.external_rtc` and added a one-shot `logRtcStatus()` at setup that prints `isEnabled`, `voltLow`, and the current RTC datetime to serial. Build clean (no RAM/flash change of note).
- Not yet done: nothing sets the RTC, so on a fresh battery the time will read as the chip's power-on default. Plan to handle that on the next iteration once we've seen what a freshly-wired unit reports.
- 0.22.13–0.22.14: added an Ex_I2C scanner. First version hung on address 1 — M5Unified's own source warns scanning addrs 0..7 stalls the I2C peripheral on ESP32-S3. Started the loop at 8 and the scan completed. Result with module unplugged: empty; with module plugged in: `device @ 0x51`. Chip present and addressable.
- 0.22.15–0.22.16: `M5.Rtc.isEnabled()` kept returning false even though raw probes at 0x51 worked at 100 and 400 kHz. Initially concluded M5Unified was buggy — wrong. The real story: `M5.Rtc` (and the `external_rtc` flag) is for boards with an RTC chip wired to the system bus alongside other peripherals; the standalone Grove RTC unit ships with its own library, `m5stack/M5Unit-RTC`, whose `Unit_RTC` class is the documented path. M5Unified's RTC class isn't intended to drive a Grove unit on the Cardputer.
- 0.22.17–0.22.19: pulled in `m5stack/M5Unit-RTC`, instantiated `Unit_RTC`. Two false starts: (1) used `Wire1` which was already claimed elsewhere, all writes failed with `NULL TX buffer`; switched to `Wire`. (2) `Unit_RTC::begin(TwoWire*, scl, sda, freq)` calls `_wire->begin(DEVICE_ADDR, sda, scl, freq)` — that's the Arduino *slave-mode* overload (first arg interpreted as slave address). Workaround: call `Wire.begin(sda=2, scl=1, 100000)` ourselves first, then `g_rtc.begin(&Wire)` single-arg.
- 0.22.20–0.22.21: added a one-shot seed from `__DATE__`/`__TIME__` when RTC year < 2025. First try used `strstr(months, __DATE__)` which can never match (looking for `"May 31 2026"` inside `"JanFeb…Dec"`); fixed by extracting the 3-char month first.
- Confirmed end-to-end: after seeding, the RTC reads `2026-05-31 16:21:47` (today's date) on a fresh boot with no re-seed firing. Time is being kept across power-cycles.
- 0.22.22: surfaced the RTC time on-device in the diagnostics overlay (bottom-right cell, replaces the `im:` IMU magnitude readout — motion already surfaces via the activity countdown above it). Refreshed once per second from `pollRtcClock()` in the main loop. Confirmed working on hardware.

## Note for follow-up

The serial monitor was unreliable for capturing early-boot output because USB CDC drops during hardware reset. Reliable capture is `pio run -e cardplayer -t upload -t monitor` — the monitor stays attached across the post-flash reset.

The `Unit_RTC` 4-arg `begin` bug (slave-mode overload) is in M5Stack's own library at https://github.com/m5stack/M5Unit-RTC — worth reporting upstream once we're confident.

The alarm-clock feature is a separate change. Foundation work (talking to the RTC, seeding the time) is complete here.

## Conclusion

HYM8563 RTC unit on Port A is talking to the player via the `m5stack/M5Unit-RTC` library, seeded once on first boot from the firmware's build timestamp, and the current time is visible on-device in the diagnostics overlay (bottom-right cell). Battery-backed time persistence depends on the user fitting the coin cell in the unit; no firmware-side concern.

The notable detour was the wrong library: `M5.Rtc` in M5Unified is for boards with a built-in RTC chip, not for the Grove RTC *unit*. The intended path for the unit is `m5stack/M5Unit-RTC`, whose header explicitly forbids co-use with M5Unified's RTC. Worth remembering for future M5Stack unit work — the "Unified" library is system-internal, the "Unit-*" libraries are for Grove accessories.

The seed path is intentionally a one-shot: if year < 2025 (chip's factory default), write the build timestamp, otherwise leave the RTC alone. This means the clock will drift by however long elapses between compile and first flash, and there's no UI to correct it. Acceptable as foundation; the planned alarm-clock change will likely want a "set time" Settings entry.

**Documentation impact:** the Diagnostics map node lists the cells in the overlay; the bottom-right cell changed from IMU magnitude (`im:`) to wall-clock (`HH:MM:SS`). Worth catching up. No dedicated RTC node yet — defer until the alarm-clock change makes RTC a first-class concept in the map.

**Changelog draft:**

> RTC. On-screen wall clock in the diagnostics overlay (bottom-right cell, replaces the `im:` IMU magnitude readout — motion still surfaces via the activity-timeout countdown above it). Reads from an M5Stack HYM8563 RTC Unit on Port A (Grove I²C, GPIO 2/1) via `m5stack/M5Unit-RTC`. Seeded on first boot from the firmware's build timestamp; the chip then keeps time across reboots, and across full power-off if the unit's CR1220 coin cell is fitted. Lays foundation for an upcoming alarm-clock feature.

