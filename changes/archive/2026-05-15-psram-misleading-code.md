# PSRAM-misleading code

**Mode:** Wander

## Intent

Explore what in the codebase led an agent to confidently — and incorrectly — assert that this device has PSRAM, and decide whether anything needs to be tightened up. The misleading signals were `g_canvas.setPsram(true)` plus an `audio_output_m5.h` and sdkconfig combination that imply PSRAM is available; the runtime reality (board declares 320 KB internal only, `CONFIG_SPIRAM is not set`) contradicts that. If `setPsram(true)` is silently falling back to internal RAM, the call name is doing harm — it suggests the canvas lives in PSRAM when in fact it occupies ~64 KB of the scarce internal heap. Worth deciding whether to remove the call, comment it as a known no-op, or restructure so the truth is loud.

## Conclusion

Removed `g_canvas.setPsram(true)` outright. The comment above the canvas-init code now states plainly that the StampS3FN8 has no PSRAM and the canvas must live in internal RAM. `sdkconfig.defaults` flags weren't touched — the `CONFIG_SOC_SPIRAM_SUPPORTED=y` lines describe chip *capability* and the `# CONFIG_SPIRAM is not set` line is the true source of "PSRAM off"; touching those is platform-IDF territory and out of scope.

Not user-visible. No changelog entry — the call was a no-op already.
