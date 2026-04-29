# Framework rebuild foundation

## Intent

Establish a reliable way to recompile the Arduino-ESP32 framework with custom sdkconfig overrides, so any future work that needs a non-default kernel option (run-time stats, PSRAM, partition tweaks, etc.) has a known-good path. The work was prompted by a desire to enable PSRAM as a memory-pressure escape hatch for the audio path; the immediate by-product is a clear answer on whether PSRAM is even feasible on this hardware.

## Approach

Use pioarduino's `custom_sdkconfig` mechanism as the override path. Trigger a framework rebuild, investigate any failures at root rather than symptom, verify on-device that the rebuilt firmware is equivalent. Use `CONFIG_SPIRAM=y` as the specific test case — both because it drives the original goal and because it's a non-trivial flag that exercises the bootloader, the heap manager, and the memory map together.

## Plan

- [x] Capture the full `wpa_supplicant` compiler failure (was truncated by pio's default reporter), identify the actual error and what defines what twice.
- [x] Move the conflicting `-DARDUINO_RUNNING_CORE=0` build flag into `custom_sdkconfig` as `CONFIG_ARDUINO_RUNNING_CORE=0` to eliminate the duplicate-define.
- [x] Diagnose the next failure (`undefined reference to __wrap_log_printf`), find the proven workaround (pass-through wrapper added to `cores/esp32/esp32-hal-log-wrapper.c`), and automate it as `scripts/patch_log_wrapper.py` invoked via `extra_scripts = pre:` so it survives package reinstalls.
- [x] Drive a `CONFIG_SPIRAM=y` framework rebuild to a clean compile + boot. Flash and read `ESP.getPsramSize()` on-device.
- [x] Determine why PSRAM doesn't initialise on this hardware. Web-research the Cardputer ADV's actual chip specification.
- [x] Revert PSRAM-specific config; keep the framework-rebuild plumbing (patch script + extra_scripts hook + `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` + `CONFIG_ARDUINO_RUNNING_CORE=0`) in place for everyday use.
- [x] Cleanup: remove the temporary diagnostic loop-print in `src/main.cpp` (memory-state lines printed for the first 10 seconds at boot).
- [x] Add a brief comment block to `scripts/patch_log_wrapper.py` documenting why it exists, what upstream issue it tracks, and when it could be removed.

## Notes

### 2026-04-28 — bring-up

**Finding 1 — `wpa_supplicant` build error has a benign root cause.** Captured the full output (was truncated by pio's reporter). The actual error is `<command-line>: error: "ARDUINO_RUNNING_CORE" redefined`. We had `-DARDUINO_RUNNING_CORE=0` in `build_flags` and `-DARDUINO_RUNNING_CORE=1` in `build_unflags`. The unflag is honoured for our app's compile but **not** the framework rebuild; the framework recompile sees both definitions and fails strict-warnings. **Fix:** set the value via `CONFIG_ARDUINO_RUNNING_CORE=0` in `custom_sdkconfig`. `esp32-hal.h` falls back to that sdkconfig when no `-D` is present, so behaviour is preserved without the duplicate-define.

**Finding 2 — `__wrap_log_printf` undefined references after the wpa_supplicant fix.** SD library's `.cpp` files emit normal `log_printf` references; the linker rewrites them to `__wrap_log_printf` via `-Wl,--wrap=log_printf` (applied by `esp_diagnostics`), but no `__wrap_log_printf` provider is being linked. Web research turned up [pioarduino issue #473](https://github.com/pioarduino/platform-espressif32/issues/473): a known bug triggered by *any* `custom_sdkconfig` (not specifically PSRAM). Upstream fix [PR #12516](https://github.com/espressif/arduino-esp32/pull/12516) was merged then reverted on 2026-04-24 due to a multi-def conflict; no follow-up yet. The proven workaround is a pass-through `__wrap_log_printf` added to `cores/esp32/esp32-hal-log-wrapper.c` (the exact code from PR #12516). `-Wl,-u,__wrap_log_printf` was tried first as a "no-source-patch" alternative; didn't work — the wrap provider that should be in `esp_diagnostics` is genuinely absent on rebuild, not just hidden by lazy archive linking. **Fix:** wrote `scripts/patch_log_wrapper.py` as a `pre:` extra_scripts hook that idempotently re-applies the patch on every build, surviving the package reinstalls that occur whenever the sdkconfig hash changes.

**Finding 3 — framework rebuild now works cleanly** with `CONFIG_SPIRAM=y` plus the patch script. RAM static usage rises modestly (the SPIRAM heap manager descriptors), build time is 60–350 s depending on what changed, and the firmware boots and runs the same as before.

**Finding 4 — PSRAM doesn't initialise on this hardware regardless of config.** Tried in turn: `CONFIG_SPIRAM=y` alone, then with `MODE_QUAD`, `TYPE_AUTO`, `SPEED_80M`, `BOOT_INIT`, `USE_MALLOC`, `USE_CAPS_ALLOC`; plus `-DBOARD_HAS_PSRAM` and `board_build.memory_type=qio_qspi`. On-device readings stayed at `ESP.getPsramSize()=0`, `heap_caps_get_total_size(MALLOC_CAP_SPIRAM)=0`, `heap_caps_malloc(1024, MALLOC_CAP_SPIRAM)=null`. Internal-free dropped by ~130 KB (the SPIRAM heap manager state allocated for nothing). With `qio_opi`, an explicit `octal_psram: PSRAM chip is not connected` log appears; with `qio_qspi`, the bootloader is silent — auto-detection finds nothing.

**Finding 5 — the Cardputer ADV does not have PSRAM.** A deeper web search settled it: eight independent sources agree the chip is **ESP32-S3FN8** — 8 MB embedded flash, **zero** external PSRAM. M5Stack's own docs and store, multiple resellers, an industry review, and two community forum threads where the missing PSRAM is the explicit topic (one is someone hardware-modding their board to add PSRAM with a soldering iron). The chip-suffix convention is the smoking gun: `FN8` = 8 MB flash + 0 PSRAM; PSRAM-bearing siblings have an `R` suffix (`FN8R2`, `N16R8`). The `psram=0` was hardware reality from the first attempt, not configuration error. The premise driving phase-1's PSRAM goal was wrong from the start — would need a hardware mod.

### Sources

- [pioarduino issue #473](https://github.com/pioarduino/platform-espressif32/issues/473) — `__wrap_log_printf` known bug.
- [arduino-esp32 PR #12516](https://github.com/espressif/arduino-esp32/pull/12516) — the upstream patch, merged then reverted.
- [arduino-esp32 PR #12288](https://github.com/espressif/arduino-esp32/pull/12288) — the related earlier fix for `__wrap_esp_log_*`, which `--wrap=log_printf` was meant to mirror.
- [M5Stack Cardputer-Adv official docs](https://docs.m5stack.com/en/core/Cardputer-Adv) — ESP32-S3FN8.
- [M5Stack community: "M5CardputerADV - add PSRAM"](https://community.m5stack.com/topic/7986/m5cardputeradv-add-psram) — hardware-mod thread confirming stock has none.

## Outcomes

- **Framework-rebuild capability**: shipped and reusable. `custom_sdkconfig` works for any flag we want; `scripts/patch_log_wrapper.py` keeps the build clean across package reinstalls. Currently used to make `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` (calibration-free CPU sampling) and `CONFIG_ARDUINO_RUNNING_CORE=0` (UI on core 0) explicit instead of relying on pioarduino's prebuilt defaults.
- **PSRAM as memory-pressure relief**: ruled out as software-impossible on this hardware. The Cardputer ADV has no PSRAM; the firmware lives within ~320 KB internal SRAM. Internal-RAM optimisation is the only path to memory-pressure relief.

A successor change (`internal-ram-optimisation`) takes the no-PSRAM finding as its starting premise and continues the memory work.

## Conclusion

Closed as a partial-but-substantive win. Phase-1's framework-rebuild capability is in place and working — the patch script + `custom_sdkconfig` path is reliable, and `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` is now explicit in the build rather than depending on pioarduino's prebuilt defaults. The PSRAM strand of the original goal turned out to be hardware-impossible on this board (ESP32-S3FN8, no external PSRAM); that's a fact recorded in Notes finding 5 and in the project's hardware-specs memory. Successor change `internal-ram-optimisation` takes the no-PSRAM finding as its starting premise.
