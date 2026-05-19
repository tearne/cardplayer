# Panel tearing investigation

**Mode:** Explore

## Intent

The ST7789 panel has no TE (tearing-effect) signal exposed, so push timing can't be synchronised to its internal scan. During heatmap pushes and diagnostics toggles, tearing manifests as a diagonal boundary — characteristic of the DMA write direction crossing the panel's refresh direction at different rates.

Two angles worth exploring: (a) raising SPI clock so a push finishes within one panel scan period (tear stays consistently trailing the scan beam), (b) measuring scan period empirically and inserting a phase-aligning delay between `waitDisplay()` return and the next push, so the write lands in the panel's blanking window.

Parked until prioritised against other work.

## Approach

### Try (A) first; (B) only if needed

Raising SPI clock is a single config change with no timing math — try first. If it shortens the push enough that the diagonal tear becomes invisible or trivial, we stop there. Phase-aligned timing (B) is more code (runtime scan-period measurement, calibrated delay) and only worth doing if (A) leaves visible residue.

### Measurement: `micros()` around the push

Instrument `presentRows` to log push duration (`pushSprite` start → `waitDisplay` return). The `waitDisplay` tail captures the panel commit; the rest is the SPI-clock-bound DMA. Comparing before / after gives a number to put alongside the visual judgement.

### Panel limit headroom

ST7789 datasheet says ~62 ns minimum write cycle (≈ 16 MHz register writes), but display-RAM bursts on similar hardware routinely run at 40-80 MHz. We'll discover what M5GFX currently sets during build, then walk up cautiously, watching for visual corruption or jitter as the failure mode.

### Out of scope

Wiring up the TE pin (hardware mod) and any change to M5GFX itself. Scope is config and small in-tree code only.

### SPI clock change is global

A higher SPI clock is set once at panel init, affecting every push. Confined-to-heatmap was an option, but tearing is mostly the heatmap and global is simpler. The walk-up criterion: raise until either the tearing is acceptable on-device OR a persistent visible artifact appears (corrupted pixels, jitter, flicker), then step back.

## Plan

Explore mode — topics rather than tasks.

**Topics**

- **Locate current SPI clock.** Find where M5GFX / M5Cardputer sets the panel's SPI write clock (during init or via a configurable bus_cfg). Record the current value as the baseline.

- **Instrument push duration.** Add `micros()` measurement around `presentRows` (and / or a couple of representative push sites). Print or capture to a diagnostics readout. Establishes a number to put alongside the visual verdict.

- **SPI-clock walk-up.** Raise the clock in steps (suggested: ~+20 MHz at a time from baseline). For each step, flash, observe heatmap tearing + look for artifacts elsewhere. Record duration and visual verdict.

- **Phase-aligned push timing (only if A is insufficient).** Measure panel scan period from back-to-back `waitDisplay` returns. Insert calibrated delay between waitDisplay and next push so the write lands in the blanking window. Code lives behind a flag so we can A/B it.

- **Verdict.** Recommendation: ship with the new SPI clock (and what value), with or without phase-aligned timing, or shelve if no combination beats the baseline meaningfully.

**Done when**

Tearing during heatmap pushes is either acceptably reduced on-device by an SPI clock change (and the chosen value is recorded), or we've concluded the achievable improvement isn't worth the complexity. Numbers for push duration before / after, and the visual verdict, captured in the Log.

## Log

- Version bumped 0.21.4 → 0.21.5. Baseline measurement build.
- Located current panel SPI clock: M5GFX autodetect for Cardputer sets `bus_cfg.freq_write = 40000000` (M5GFX.cpp:1872). That's the baseline.
- Reconfigured at startup via `M5Cardputer.Display.getPanel()->getBus()->setClock(PANEL_SPI_HZ)`, currently set to the same 40 MHz to confirm the override path works before walking it up.
- Added push-duration instrumentation in `presentRows` — rolling 32-sample average, Serial-dumped only when a visualisation is active. Need monitor open during flash to capture numbers.
- 0.21.5 baseline at 40 MHz: avg push ~8.5 ms, occasional spikes to ~11 ms. About half a panel scan (60 Hz → 16.7 ms scan). The variable portion is `waitDisplay` phase against the scan; SPI DMA is the rest.
- 0.21.6: jumped straight to 80 MHz (M5GFX docs / ST7789 hardware commonly tolerate this). Skipping smaller steps; if 80 MHz produces no artifacts we land here; if it does, step back.
- 0.21.6 numbers: push avg ~4.5 ms at 80 MHz, no visible artifacts. Confirms `waitDisplay` is only ~0.5 ms (= DMA tail), not panel commit — most of the original 8.5 ms was the SPI DMA itself. Visual tear improvement was minor: with push rate (60 Hz) matching panel scan (~60 Hz), tearing is at a roughly stationary diagonal regardless of push duration.
- 0.21.7: added a `PUSH_PHASE_DELAY_US` knob to insert a calibrated delay between `waitDisplay` return and the next push. Currently 0. To sweep: rebuild with values like 2000, 4000, 8000, 12000 and observe whether the diagonal moves to a less prominent position or breaks up.
- 0.21.8 (delay 4000 µs) and 0.21.9 (delay 8000 µs): no significant visual change. Confirms the diagonal isn't where the visible jitter is coming from. Pipeline walk-through suggested integer truncation of `disp_abs` and clamping against bursty `abs_head` are likelier culprits for what reads as "jitter" — out of scope here.
- 0.21.10 / 0.21.11: cleanup. Phase delay removed; `push avg us` Serial instrumentation removed (was spamming during playback). Final state: SPI clock at 80 MHz, no other changes from baseline.

## Conclusion

SPI clock 40 → 80 MHz halved push duration (~8.5 → ~4.5 ms measured) with no visible artifacts. Phase-aligned push timing (option B) was added, swept (0 / 4 / 8 ms delays), and didn't appreciably change the visible tearing. The investigation revealed the diagonal tear isn't the dominant contributor to perceived scroll jitter — the likelier culprits are integer truncation of `disp_abs` (1-vs-2 px-step inconsistency per render) and `disp_abs` clamping interacting with bursty `abs_head` advances. Both warrant a follow-up change.

**Documentation impact:** none.
