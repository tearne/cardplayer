# Leveller distortion on loud passages

**Mode:** Explore

## Intent

With leveling on, loud passages distort. The limiter isn't catching peaks cleanly — they reach the ceiling and clip rather than being pulled down smoothly ahead of time. The user wants loud parts handled without distortion: rework how the limiter anticipates and tames peaks, revisit which settings are exposed and the defaults so the result is clean, and make the amplification visualisation clearer (vertically scaled) so its effect is legible while tuning.

## Approach

### Rework into a true lookahead peak limiter

Detect the peak across the whole lookahead window, not just the current sample, and pull the gain down so it reaches the needed reduction *before* the peak reaches the output — leaving the −1 dBFS clamp as a rare safety net rather than the primary mechanism. *Reason: today the gain is driven by the instantaneous sample and a one-pole attack that reaches only ~63 % of the needed reduction within the window, so peaks arrive under-attenuated and the clamp hard-clips them — that clamp-clipping is the distortion.*

**Detail** — maintain a sliding-window minimum of the per-sample required gain (`min(1, ceiling/|driven|)`) across the delay line via a monotonic deque (O(1) amortised); the envelope fast-attacks down to that windowed target and releases up slowly. Because the target already reflects the upcoming window, the gain is always low enough by the time a sample is emitted.

### Expose the full knob set, most-relevant first

All five parameters become Settings rows in the Leveling sub-menu, ordered by everyday relevance: **Drive gain, Release, Attack, Lookahead, Ceiling** (below the Leveling toggle, above Reset / Back). Each gets a sensible range and a default tuned by ear; the lookahead may grow beyond today's ~3 ms (the user is fine with the extra latency, still negligible against the ~220 ms decode-ahead). The default drive is **+12 dB** (raised from +6 during tuning — it lifts quiet speech more and stress-tests the limiter harder). *Reason: the user wants full control with the knobs they reach for most at the top; the rest are fine-tuning that lives lower.* New NVS keys + Reset-all defaults follow for the added parameters.

### Make the amplification trace use the vertical space

The line was often hard to see moving. Spread the displayed range across more of the waveform pane (rather than the cramped lower quarter of the upper half it occupies under the fixed 0…+24 dB mapping) so its movement reads clearly — exact mapping (full-height, and/or scaling the band to the working range) tuned by eye. *Reason: the goal is legibility of the line's motion, not a particular dB scale.*

## Plan

**Topics**

- Rewrite `LoudnessLimiter` as a lookahead peak limiter (windowed peak detection + attack that reaches target within the window + slow release); keep the clamp as a safety net; confirm it stays lock-free and within the per-sample budget.
- Tune defaults and the exposed-knob set by ear on loud passages — settle drive default, lookahead, attack, and which knobs reach Settings.
- Rescale the waveform amplification trace so it reads clearly at the chosen default drive.
- Carry any Settings / NVS / map changes that the knob-set decision implies.

**Done when** loud passages play without audible distortion at the default settings, the five exposed knobs (Drive, Release, Attack, Lookahead, Ceiling) let the user trade smoothness against latency, and the amplification trace clearly shows the limiter working.

## Log

- Limiter reworked (kept the feed-forward delay structure, which is a correct lookahead limiter) — the fix was decoupling attack from the lookahead: attack is now its own fast coefficient (default 1 ms within a 5 ms lookahead), so the gain reaches the needed reduction before a peak is emitted instead of the old one-pole reaching only ~63 %. Detection stays instantaneous (per-sample) rather than a windowed-min deque: with attack ≪ lookahead and release ≫ lookahead, the slow release holds the reduction across the delay, so peaks are caught without the deque's extra ~5 KB RAM and complexity. Clamp is now a true safety net. If residual distortion survives tuning, escalating to a sliding-window-min deque is the fallback.
- Exposed five knobs in the Leveling sub-menu, most-relevant first: Drive, Release, Attack (0.5–10 ms, half-ms steps), Lookahead (1–12 ms), Ceiling (−0.5…−6.0 dB, half-dB steps). New NVS keys `lvlat`/`lvlla`/`lvlce`, Reset-to-default and Reset-all updated. The sub-menu gained scroll-to-cursor since 8 rows + title overflow the screen at the default font.
- `LOOKAHEAD_MAX` grew 256 → 640 floats (covers 12 ms @ 48 kHz); `g_out` static rises ~1.5 KB — negligible, and well within the 24 KB just freed from chess.
- Amplification trace now scales to the current drive (full drive ≈ top of the upper half, dipping as the limiter works) instead of a fixed 0…+24 dB, so the line uses the whole band — addressing "hard to see it move."
- Built clean at 0.26.0; awaiting hardware test on loud material.
- Raised the default drive +6 → **+12 dB** (0.26.1, user tuning). Affects fresh installs / Reset-all only; an existing saved value is untouched — to hear +12 on a device that already saved +6, adjust the row or Reset all.

## Conclusion

Built the lighter of the two designs the Approach floated: the distortion was root-caused to the attack coefficient matching the lookahead (reaching only ~63 % of the needed reduction in time), so it was fixed by decoupling attack as its own fast coefficient with instantaneous detection — *not* the windowed-peak deque the Approach's Detail described. The deque's stronger guarantees (clean at any attack, attack as a pure smoothness control) were instead written up as the deferred `limiter-windowed-peak` proposal, to adopt only if tuning shows the simple version's clean zone is too narrow. Five knobs now reach the Leveling sub-menu (Drive, Release, Attack, Lookahead, Ceiling) with scroll-to-cursor; default drive raised to +12 dB; the amplification trace scales to the drive setting. The Leveling menu's Back row was left in place on purpose — removing Back across all menus is the separate `drop-menu-back-rows` proposal. Shipped 0.26.1.
