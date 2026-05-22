# Visualisation Latency

**Mode:** Explore

## Intent

The waveform and spectrum overlays lag noticeably behind what's audible. Investigate where that delay comes from and whether it can be tightened so the visualisations track playback closer to real time.

## Approach

### Where the delay comes from

The viz cursor is anchored to `abs_head` — the position the decoder has committed into the ring — not to what's audible. Audible playback runs ~220 ms behind decoder-commit (pre-buffer + 3-slot output ring + 2-deep speaker queue, per [Audio Pipeline](../../map.md#audio-pipeline)). The viz then subtracts `VIZ_LOOKAHEAD_MS = 500` ms of headroom before rendering, placing the displayed column at roughly `abs_head − 500 ms`. Net display-vs-audible offset ≈ 500 − 220 = ~280 ms *behind* audible. The code comment at the constant claims the design target was 250 ms ("lands display close to audible"); the constant is currently double that — so part of the delay is a stale-comment / drifted-tuning gap, not an inherent pipeline limit.

### Two routes to reduce it

- **Tune the headroom constant.** `VIZ_LOOKAHEAD_MS` exists purely to keep `target_max` from overshooting an unwritten slot during decoder bursts (FLAC frames ~11 cols ≈ 90 ms at typical rates). Dropping it toward `tap_to_speaker + burst_headroom` (~220 + 100 ≈ 320 ms → ~100 ms behind audible, or further) is a one-line change with bounded risk: if it's too low, the existing `target_max` cap and the safety-snap path catch it. Cheap to bisect.

- **Anchor on audible position instead.** Track samples consumed by the speaker (a `samples_played` counter incremented in the I2S write path), and anchor the prediction model on that instead of `abs_head`. Yields zero display-audible offset by construction, independent of pre-buffer / ring depth. Bigger surgery — new counter, different prediction model — but the right shape if the constant-tune turns out to leave visible delay.

### Tuning strategy

Visual-bisect by ear-and-eye, no instrumentation. Lowering `VIZ_LOOKAHEAD_MS` slides the displayed column forward in time. Slightly *early* (viz a few tens of ms ahead of audible) is acceptable — even pleasant on a player where the viz is anticipating, not reporting. So the floor isn't "tap_to_speaker + safety"; it's "wherever it looks good without pulsation from the target_max cap firing on decoder bursts".

### Scope

In-scope: tune `VIZ_LOOKAHEAD_MS` (and any directly coupled constants — e.g. `VIZ_PRED_LAG_REANCHOR_COLS` if reducing lookahead pushes us closer to its threshold) by listening on representative tracks; update the stale comment alongside the value.

Out of scope: the audible-anchor route. If a tuned constant doesn't land the viz close enough to audible, we'll open a follow-up change.

## Plan

**Topics**

- Bisect `VIZ_LOOKAHEAD_MS` downward from 500, flashing and listening on representative tracks (something rhythmic for waveform, something tonal for spectrum) until the viz visibly tracks audible or runs a touch ahead.
- Watch for pulsation / cap-firing artefacts at low values — that's the floor.
- If lowering lookahead crowds `VIZ_PRED_LAG_REANCHOR_COLS`, adjust it too.
- Update the comment at `VIZ_LOOKAHEAD_MS` to match the chosen value and reasoning.

**Done when**

The visualisations track audible playback closely on representative tracks, with no observable pulsation or column-drop artefacts.

## Log

- Visual bisect settled at `VIZ_LOOKAHEAD_MS = 175` (down from 500). Tried 250 → 200 → 150 → 175. Net change: viz moves from ~280 ms *behind* audible to ~45 ms *ahead* of audible.
- `VIZ_PRED_LAG_REANCHOR_COLS` left untouched — no pulsation observed at 175 ms.
- Comment block above the constant rewritten to reflect the new value and the visual-bisect rationale.
- Version bumped to 0.21.68.

## Conclusion

Visual bisect of `VIZ_LOOKAHEAD_MS` from 500 to 175 ms — viz now leads audible by ~45 ms instead of trailing by ~280 ms. No deeper audible-anchor work needed.
