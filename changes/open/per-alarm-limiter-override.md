# Per-alarm limiter override

## Intent

Fall asleep to an audiobook with leveling on, wake to music with it off. Each alarm should carry its own choice about loudness leveling so the limiter state at fire time follows the alarm rather than whatever was last set by hand.

Each alarm gains a tri-state field — **leave / force on / force off** (default *leave*, so no behaviour change for existing alarms) — applied to the limiter at fire time via its on/off setter. Adds an alarm-editor row and one NVS key per slot (e.g. `aN_l`).

Restore-on-dismiss follows the alarm clock's "snapshot prior state on fire" decision: if the alarm restores prior volume on dismiss, it restores the prior limiter state too; otherwise the alarm's choice simply persists.

Spun off from `loudness-limiter` (archived once concluded), whose Asides parked this as a follow-on. Both the limiter and the alarm system now exist, so this is buildable.
