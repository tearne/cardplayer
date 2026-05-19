# PlatformIO pre-build hook — empties ESP8266Audio source files we don't
# use that either fail to compile under pioarduino / Arduino-ESP32 v3, or
# pull in ESP-IDF subsystems that conflict with M5Unified's I2S setup.
#
# WHY THIS EXISTS
# ---------------
# Two classes of files need neutralising:
#
# 1. HTTP/ICY streaming: AudioFileSourceHTTPStream and AudioFileSourceICYStream
#    pull in WiFi / WebClient APIs that don't compile against this platform's
#    header set. The project doesn't use webradio streaming (SD-card files
#    only), so they can be excluded.
#
# 2. I2S / SPDIF output: AudioOutputI2S, AudioOutputI2SNoDAC, and
#    AudioOutputSPDIF include `driver/i2s.h` (legacy). M5Unified uses
#    `driver/i2s_std.h` (new, ESP-IDF v5). Linking both subsystems trips
#    ESP-IDF's `check_i2s_driver_conflict` during `do_global_ctors`,
#    aborting the boot before `setup()` runs. The project drives audio
#    through `audio_output_m5.h` (M5.Speaker.playRaw), so these I2S
#    backends are never instantiated; emptying their TUs prevents the
#    legacy I2S driver from being linked at all.
#
# Excluding these files lets us pin a stock release of ESP8266Audio
# rather than fork or vendor the whole library.
#
# WHAT THE PATCH DOES
# -------------------
# Locates the ESP8266Audio library under the project's libdeps directory
# and overwrites each listed file with a single-line marker comment.
# The library's used files do not include any of these headers (verified
# at the time of writing).
#
# IDEMPOTENCY
# -----------
# Re-running is safe: the script checks for the marker before overwriting.
# Survives package reinstalls and library version bumps within ESP8266Audio
# 1.9.x. If a future ESP8266Audio version restructures the source tree,
# the missing-files branch logs a warning rather than failing the build.

Import("env")

import os
import glob

MARKER = "// excluded by scripts/exclude_esp8266audio_http.py — webradio not used\n"

LIB_FILES = [
    "AudioFileSourceHTTPStream.cpp",
    "AudioFileSourceHTTPStream.h",
    "AudioFileSourceICYStream.cpp",
    "AudioFileSourceICYStream.h",
    "AudioOutputI2S.cpp",
    "AudioOutputI2S.h",
    "AudioOutputI2SNoDAC.cpp",
    "AudioOutputI2SNoDAC.h",
    "AudioOutputSPDIF.cpp",
    "AudioOutputSPDIF.h",
]

libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
pioenv = env.subst("$PIOENV")

# ESP8266Audio installs as ESP8266Audio/ (registry name); allow for any
# casing or version suffix variation by globbing.
candidates = glob.glob(os.path.join(libdeps_dir, pioenv, "ESP8266Audio*", "src"))

if not candidates:
    print("[exclude_esp8266audio_http] WARNING: ESP8266Audio src dir not found under %s/%s" %
          (libdeps_dir, pioenv))
else:
    src_dir = candidates[0]
    for name in LIB_FILES:
        path = os.path.join(src_dir, name)
        if not os.path.exists(path):
            print("[exclude_esp8266audio_http] WARNING: %s not present" % path)
            continue
        with open(path, "r") as f:
            current = f.read()
        if current == MARKER:
            continue
        with open(path, "w") as f:
            f.write(MARKER)
        print("[exclude_esp8266audio_http] emptied %s" % name)
