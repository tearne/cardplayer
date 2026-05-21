# Restore speaker output ring to 3 slots

**Mode:** Wander

## Intent

The Tier 1 RAM audit dropped speaker output staging from 3 slots to 2 to save ~3 KB. Audio is now audibly crackly. The third slot was margin against the speaker library reusing a slot while it was still being played; 2 slots is too tight. Revert that one knob — accept the 3 KB regression, keep the other Tier 1 savings.

## Conclusion

Three things ended up in this Wander:

1. **`RING_COUNT` restored to 3** — fixes the audio crackle that appeared after the prior Tier 1 RAM trim. Audio task stack stayed at 6 KB (peak 34 %, plenty of headroom).
2. **Pre-buffer bumped to 9000, then reverted to 7000** — bigger pre-buffer didn't change the `buf%` dips because the metric wasn't measuring what we thought it was.
3. **`buf%` diagnostic reworked.** Old version: percentage of a 2 ms cap that the last `playRaw` call blocked — a one-shot snapshot of a binary "queue full / queue not full" state, with the 4 Hz diag sampler catching false dips. New version: 100 × (minimum pre-buffer fill since last 250 ms sample) / capacity. Reflects worst-case decoder headroom in the window. Reads ~78 % under normal playback in test.

Net RAM: returns 3 KB of the prior Tier 1 trim (the speaker ring restoration). The pre-buffer trial was fully reverted.

**Documentation impact:** the `Diagnostics` node of `map.md` describes the `buf` readout. The semantics changed — worth updating as a per-node negotiation post-archive.