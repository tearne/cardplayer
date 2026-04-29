# PlatformIO pre-build hook — patches the Arduino-ESP32 framework so it
# links cleanly when we use `custom_sdkconfig` to override the prebuilt
# kernel options.
#
# WHY THIS EXISTS
# ---------------
# `esp_diagnostics` applies `-Wl,--wrap=log_printf` unconditionally (its
# CMakeLists adds it whenever `CONFIG_LIB_BUILDER_COMPILE=y`, which
# arduino-esp32's defconfig sets). That linker flag rewrites every
# `log_printf` call site to `__wrap_log_printf`. The wrap *provider* lives
# inside `libespressif__esp_diagnostics.a` and works fine on the prebuilt
# framework path. The moment any `custom_sdkconfig` line is added to
# platformio.ini, pioarduino rebuilds the framework — and the wrap
# provider gets dropped from the link by static-archive lazy linking,
# leaving the SD library, Wi-Fi (if enabled) and others with undefined
# references to `__wrap_log_printf`.
#
# Tracked upstream as:
#   - pioarduino issue #473  https://github.com/pioarduino/platform-espressif32/issues/473
#   - arduino-esp32 PR #12516  https://github.com/espressif/arduino-esp32/pull/12516
#     (the upstream fix — merged 2026-04-24, reverted same day on a
#     multi-definition conflict with esp_diagnostics. No follow-up yet.)
#
# WHAT THE PATCH DOES
# -------------------
# Appends a pass-through `__wrap_log_printf` to
# `cores/esp32/esp32-hal-log-wrapper.c` that simply forwards to
# `log_printfv`. Same code as PR #12516. Outside the
# `CONFIG_DIAG_USE_EXTERNAL_LOG_WRAP` guard so it's always defined when
# our rebuild path is in use; the multi-definition that bit upstream
# doesn't trigger here because lazy linking has already dropped the
# competing provider.
#
# WHEN IT CAN BE REMOVED
# ----------------------
# When pioarduino ships a release whose framework rebuild produces a
# linkable `__wrap_log_printf` (the upstream PR #12516 follow-up, or an
# equivalent fix). At that point the build will succeed without this
# patch and the script becomes a no-op (it already short-circuits when
# the symbol is present). Safe to leave in place even after upstream
# fixes.
#
# IDEMPOTENCY
# -----------
# Re-running is safe: the script checks for `__wrap_log_printf` in the
# wrapper file before appending. Survives package reinstalls, which
# pioarduino triggers whenever the sdkconfig hash changes.

Import("env")

import os

WRAPPER_FILE = os.path.join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "cores", "esp32", "esp32-hal-log-wrapper.c",
)

PATCH = '''
/* ---------- pioarduino-issue-473 workaround (auto-patched) ----------
 * Pass-through wrapper for log_printf so the linker -Wl,--wrap=log_printf
 * (applied via esp_diagnostics with CONFIG_LIB_BUILDER_COMPILE=y) resolves
 * cleanly on framework-rebuild paths where the prebuilt provider archive
 * is dropped by lazy linking. Re-applied by scripts/patch_log_wrapper.py
 * on every build.
 */
#include <stdarg.h>
extern int log_printfv(const char *format, va_list arg);
int __wrap_log_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int len = log_printfv(format, args);
    va_end(args);
    return len;
}
'''

if os.path.exists(WRAPPER_FILE):
    with open(WRAPPER_FILE, "r") as f:
        content = f.read()
    if "__wrap_log_printf" not in content:
        with open(WRAPPER_FILE, "a") as f:
            f.write(PATCH)
        print("[patch_log_wrapper] applied __wrap_log_printf to %s" % WRAPPER_FILE)
    else:
        print("[patch_log_wrapper] already present")
else:
    print("[patch_log_wrapper] WARNING: wrapper file not found at %s" % WRAPPER_FILE)
