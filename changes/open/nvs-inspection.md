# NVS inspection

## Intent

Persistent state lives in NVS (player settings, playhead, chess board, etc.) but there's no way to see what's actually in there short of pulling the partition off the device with Espressif tools. The user wants a routine way to peek — at minimum to spot stale keys from old versions, confirm a save actually happened, and generally treat NVS as something maintainable rather than opaque.

Likely shape: a serial command (in the spirit of the fuzzy harness) that walks every namespace and key in NVS and prints `namespace.key = value` to the monitor. Optional follow-up: a `delete` form for removing specific keys without a full reset.
