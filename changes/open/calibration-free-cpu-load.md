# Calibration-free CPU load

## Intent

The diagnostics row's per-core CPU load currently self-calibrates against the highest idle rate observed since boot, so each core's reading is unreliable until it has seen one mostly-idle second. The user wants direct, real-time CPU load measurement that's accurate from the very first sample — no warmup, no "max-rate-seen" reference, just microseconds of idle time over microseconds of wall time.

The path is to enable FreeRTOS run-time stats in the build, which exposes per-core idle counters in real microseconds. That requires rebuilding Arduino-ESP32 from source with a custom `sdkconfig.defaults` (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`, `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`); the prebuilt FreeRTOS that ships with PlatformIO has both options off, which is why the current implementation uses an idle-hook rate counter as a workaround.
