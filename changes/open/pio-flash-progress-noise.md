# PlatformIO flash progress noise

## Intent

PlatformIO / esptool's flash progress output writes a fresh line per progress update instead of updating one line in place — so the terminal scrolls past dozens of `Writing at 0x... [████░░░░░] N%` lines for every flash. This is a visual nuisance that buries the pre- and post-flash output. Worth investigating whether this is a configurable behaviour (esptool flag, environment variable, monitor filter) or pioarduino-specific output handling that we can quiet down.
