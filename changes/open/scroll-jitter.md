# Visualisation scroll jitter

**Mode:** Explore

## Intent

The scrolling visualisation feels slightly jittery — not the panel tearing chased and answered in the previous change, something subtler. Two likely contributors from the pipeline walk-through:

**A.** `disp_abs` advances by `elapsed_us / col_period_us` (a fractional double) then truncates to integer for slot indexing. With render threshold = 2 × col_period and actual elapsed slightly variable, per-render advance is 2.00 to 2.12 cols → truncated to 2 or 3. A 1-pixel inconsistency every render.

**B.** `disp_abs` is clamped to `abs_head − lookahead`. The audio task commits in clumps (decoder bursts), so `abs_head` rises in clumps; `disp_abs` brief-clamps when audio's behind, then free-runs when a burst arrives. Push rate vs commit rate phasing oscillates.

Explore how to eliminate or reduce both. Open: render-tick-locked deterministic advance, sub-pixel rendering, abs_head smoothing, or revisiting the wall-clock cursor design.

## Approach

### Deterministic per-render advance (addresses A)

Replace `prev_us = now_us; advance = elapsed_us / col_period_us` with `prev_us += threshold_us; advance = VIZ_COLS_PER_PUSH` (exact integer). Each render moves `disp_abs` by the same constant integer; the 1-vs-2 pixel inconsistency disappears. Render *timing* is still wall-clock-driven (next poll fires once `now ≥ prev_us + threshold`), but the value applied is deterministic. Safety net: if `abs_head − disp_abs` ever exceeds a few cols, snap forward — covers a stalled main loop.

### Predicted abs_head if A leaves jitter (addresses B)

Track `(anchor_abs, anchor_us)` of the last abs jump. Compute `predicted_abs = anchor_abs + (now − anchor_us) / col_period_us` — monotonic and smooth even when actual `abs_head` rises in clumps. `target_max = predicted − lookahead`. Re-anchor when actual abs catches or overtakes prediction. Freeze prediction if it would overshoot actual by more than a small bound (handles pause / underrun).

This is deferred — apply only if A doesn't visibly close the gap.

### Verification

Visual judgement is primary. If subjective improvement is unclear, add a per-render-advance histogram (printed periodically) so we can see whether the advance is actually constant now.

## Plan

Explore mode — topics rather than tasks.

**Topics**

- **Deterministic advance (A).** Rewire `pollVisualisation` so `prev_us += threshold_us` and per-render advance is exactly `VIZ_COLS_PER_PUSH`. Safety snap if `abs_head − disp_abs` exceeds a few cols.

- **On-device verdict on A alone.** Does the scroll read as smoother? If yes, stop. If no, continue.

- **Predicted abs_head (B), only if needed.** Add the anchor / predict / bound mechanism for `target_max`. Re-verify on-device.

- **Diagnostic histogram, if subjective verdict is unclear.** Periodic Serial dump of per-render advance counts and (if B applied) predicted-vs-actual abs gap.

**Done when**

The visible jitter during scrolling is acceptably reduced or we've concluded the remaining residue isn't worth further work. Written verdict.

## Log

- Version bumped 0.21.11 → 0.21.12.
- 0.21.12: implemented topic A (deterministic per-render advance). `pollVisualisation` now steps `prev_us` by exactly `threshold_us` (not `now_us`) and grows `disp_abs` by exactly `VIZ_COLS_PER_PUSH` per render. Safety snap if `disp_abs` falls more than 8 cols behind `target_max`. Each render now shifts the visible window by the same integer count.
- 0.21.12 verdict: pulsation, "potentially smoother per step" but strong overall pulsation. Matches the predicted symptom of A leaving B exposed — bursty `abs_head` advance pulses through `target_max`, causing periodic clamp-and-free-run.
- 0.21.13: implemented topic B (predicted abs_head). Anchor pair updates whenever the actual `abs_head` changes; between updates a linear-in-wall-clock prediction provides a smooth target. Capped to actual + 5 cols so prediction freezes during pause / stalls. `target_max = predicted - lookahead`.
- 0.21.13 verdict: pulsation persists, just as strong. Bug found in B: re-anchoring on every observed `abs` change made predicted snap to actual on each burst — predicted was essentially a 1-step-lagged copy of actual, no smoothing happening. 0.21.14: anchor set ONCE at activation, predicted grows purely linearly from there. Cap raised to `actual + lookahead - 1` (was actual + 5) so the prediction has headroom to ride out burst arrivals. Drift correction re-anchors only if predicted falls > 20 cols behind actual (covers sample-rate mismatch over long playback).
- 0.21.14: still pulsating at 5-10 Hz. User's hunch correct — parameters were the issue. Cap on predicted was set at `actual + lookahead - 1 ≈ 12`, which is right at the edge of FLAC burst size (~11 cols). Predicted oscillated 0..11 cols ahead of actual, brushing the cap on each cycle. When cap fires, `target_max = actual` flatlines; `disp_abs` (growing at constant col rate via A) catches up and clamps; brief stall = the visible pulse, at exactly the FLAC frame rate.
- 0.21.15: raised `VIZ_LOOKAHEAD_MS` 110 → 250 ms. Lookahead now ~30 cols, well above any normal burst size. Predicted's cap should never fire; `target_max` should grow smoothly. Side effect: display lags actual commits by ~30 cols ≈ 250 ms, aligning it close to what's coming out of the speaker (the FFT tap leads audible by ~220 ms).
- 0.21.15, 0.21.16 (500 ms), 0.21.17 (cols-per-push 1), 0.21.18 (back to 2): pulsation persists at 5–10 Hz regardless. Even with cap effectively disabled (huge lookahead).
- 0.21.19 added timing instrumentation. Compose ~9 ms, push ~6 ms, very stable. Render time fits the 16.6 ms threshold but with little slack — and the *interval* between renders is bound to the main loop's variance. So the cursor mechanism is fine; the constraint is compose cost dominating render budget.
- 0.21.20: structural fix. Added persistent `g_viz_sprite` (8 bpp, browser-inner size). Each render: scroll left by `VIZ_COLS_PER_PUSH`, draw only the new rightmost columns. Compose work drops from 240 × 28 = 6720 fillRects to ~28-56 fillRects + an 8 bpp memmove. Sprite pushed directly to display (skipping g_canvas conversion). On mode change / size change / snap, dirty flag forces a full sprite redraw.

