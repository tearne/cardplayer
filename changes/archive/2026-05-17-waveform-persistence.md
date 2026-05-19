# Waveform persistence and window size

## Intent

The waveform overlay is currently dismissed by any meaningful keypress — including transport (`space`, `{`, `}`, `'`, `[`, `]`, `1`-`0`) and volume (`-`, `=`) keys. That makes it impractical to use waveform mode while actually controlling playback: glancing at the visualisation and pausing or skipping immediately blanks it. The user wants those keys to act on playback without dismissing the overlay, so the waveform stays up across a normal listening session. Dismissal should still happen on letter keys (the natural way to escape into another mode) and explicit gestures (`Fn+\`` esc, `Ctrl+W` toggle off).

The user would also like to widen the *timespan* the waveform represents — each on-screen column should cover more milliseconds of audio, so the wave scrolls more slowly and a longer slice of the track is visible at once. Today the timespan is fixed by the audio pipeline's look-ahead: ~276 ms total (220 ms of look-ahead occupies the 80 % of screen right of the playhead).

## Approach

### Timespan as a user setting

Add a waveform-timespan tunable with discrete steps: `0.5s, 1s, 2s`. Default `1s`, applied uniformly whether the overlay opens via `Ctrl+W` or the auto-waveform setting. Persisted alongside the other settings. Exposed as a numeric row in the [Settings](#settings) screen — `,` / `/` step through the list — slotted next to the Auto waveform toggle.

The 5 s option is dropped because — see the layout decision below — supporting it would require ~74 KB of additional pre-buffer (≈ 1 s of look-ahead at 44.1 kHz mono int16) and a ~1 s lengthening of the track-switch gap. 2 s is the largest step that's worth the cost.

`waveformSamplesPerCol()` is currently derived from a hardcoded `LOOKAHEAD_MS = 220` and `FUTURE_COLS = 192`. Repoint it at the user-chosen timespan instead: `samples_per_col = (timespan_ms × sample_rate) / (1000 × WV_COLS)`. The same 240-column ring buffer (480 bytes total) now spans the chosen timespan; longer timespans simply accumulate more samples per slot before emitting.

### Fixed 80 / 20 layout

Playhead stays at 80 % of screen width from the left — past on the left, future on the right — regardless of timespan. This is the user's mental model: a wider window of past audio with a small constant preview ahead.

To honour the ratio at the widest 2 s step, the audio pipeline's total look-ahead must be ≥ 400 ms (20 % of 2 000 ms). Today it's ~220 ms (~150 ms pre-buffer + ~70 ms downstream output ring / speaker queue).

### Deepen the pre-buffer

Bump `PREBUF_SAMPLES` from 7 000 (~158 ms at 44.1 kHz, ~146 ms at 48 kHz) to 16 000 (~363 ms at 44.1 kHz, ~333 ms at 48 kHz). Adding the ~70 ms downstream gives ~400–430 ms of total look-ahead — exactly enough for 2 s timespan at 80 / 20.

RAM cost: 16 000 × 2 bytes = 32 KB (was 14 KB) → +18 KB to DIRAM `.data`. Pushes total DIRAM use from ~39 % to ~44 %. Comfortable.

Track-switch cost: the decoder must fill ~330 ms of pre-buffer before the audio task ships the first slot. The track-start latency grows roughly proportionally — accept this as the price of the wider view.

### Ring flush on timespan change

The 240-column ring is in units of "samples_per_col" — when that changes, the existing contents represent a different time scale than the new draw routine expects. Clearing min/max to silence (zero) and resetting `head` makes the next ~0.5–2 s of audio refill the buffer with consistent column widths. Acceptable: the user just chose this setting, a momentary scroll-in of fresh data is expected feedback.

### Pass-through and dismissal keys

Keys that act on playback or device feel — and therefore make sense to use *while watching* the waveform — fire their normal action without dismissing the overlay:

- `space` — pause / resume
- `{` / `}` — prev / next track
- `'` — jump to playing
- `[` / `]` — seek (held)
- `1`–`0` — jump to %
- `-` / `=` — volume
- `Fn+=` / `Fn+-` (and shifted) — brightness

Dismissal continues on everything else meaningful: letter (opens fuzzy), `Ctrl+W` (explicit toggle off), `Fn+\`` (esc), `?` (Settings), `Del` (reset modal), `enter`, navigation (`;`, `.`, `,`, `/`), structural (`+`, `_`, `` ` ``, `\`, `~`).

### Held-seek through the overlay

`pollSeekKeys` exits early when any overlay is active. The waveform "overlay" is conceptually a view *of* playback, so seeking should pass through. Relax the guard to allow seek while waveform is on.

### Window scope

The overlay continues to use the current browser area (between header and footer); the user did not ask for a taller window, only a wider timespan. Header and footer remain so transport state, version, battery, and diagnostics stay visible during a typical listening session.

## Plan

- [~] Waveform timespan: state, default 1s, persistence (load / save / reset / clamp) — built then reverted
- [~] Bump `PREBUF_SAMPLES` from 7000 to 16000 — built then reverted (ran into runtime heap pressure)
- [~] Replace `LOOKAHEAD_MS`-based `waveformSamplesPerCol()` with timespan-driven computation — reverted
- [~] Playhead fixed at 80% of screen width — built then reverted (user wanted 20%, then RAM gave out)
- [~] Reset ring buffer when timespan changes — reverted along with the setting
- [~] Settings row "Waveform span" — reverted
- [~] Settings adjust + display wired — reverted
- [x] Waveform input branch: pass-through for space, `{`/`}`, `'`, `[`/`]`, `1`-`0`, `-`/`=`, `Fn+=`/`Fn+-`
- [x] Waveform input branch: dismiss on letter, enter, navigation, structural, `?`, `Del`, `Ctrl+W`, `Fn+\``
- [x] `pollSeekKeys` allows held-seek through the waveform overlay (already worked; confirmed)
- [x] Bump APP_VERSION and add CHANGELOG entry

## Conclusion

Shipped as 0.19.8. Final scope is much narrower than the Plan: the keyboard pass-through behaviour (transport / volume / brightness keys keep the overlay up) is the only piece that survived. The wider-timespan goal — and the pre-buffer expansion, playhead repositioning, smoothing logic, and Settings row that came with it — all got rolled back when the runtime heap turned out to be the binding constraint, not just static DIRAM.

Two technical lessons captured in the Log:

1. The `+18 KB / +66 KB` static-RAM accounting we used during planning didn't reflect the real cost: heap budget shrinks too, and FLAC working buffers + lazy-loaded fuzzy index pushed runtime usage to 96 %.
2. Even with the headroom, the time-driven smoothing (`g_waveform_virt_abs` capped at `abs_head - SAFETY`) didn't actually smooth the visible scroll — virt mostly tracked the cap, which moves with the bursty actual head. A proper fix would need virt to advance under-cap continuously, which means accepting that the rightmost cols sometimes show stale or silent data.

Both points feed directly into the next change.

Changelog: rolled into a single 0.19.0 line covering the pass-through behaviour only.

## Log

- Held-seek through the overlay was already working — `pollSeekKeys` gates on `overlayActive()`, which only covers Settings / key-reference / reset modal, not waveform. No change needed; task ticked as a confirmation.
- RAM use after build: 25.1 % (was 19.6 %). +18 KB matches the predicted pre-buffer growth (7 000 → 16 000 × 2 bytes). Flash unchanged at 27 %.
- The `setWaveformTimespanMs` setter clears the ring buffer to silence on every call so the next refill starts at the new column width — applies cleanly mid-playback.
- 0.19.1: misunderstanding on the past/future ratio. Approach captured "80 % past" from the user's earlier message; on first test they reported wanting the opposite (20 % past / 80 % future, matching the original 0.18.x layout). Playhead reverted to 20 % from left.
- 0.19.8: Rolled back the wider-timespan work. `PREBUF_SAMPLES` returned to 7 000; `waveformSamplesPerCol` reverted to the `LOOKAHEAD_MS=220` formula; the `Waveform span` setting and its persistence removed; the `abs_head` counter and `g_waveform_virt_abs` smoothing removed. Only the pass-through key behaviour (the original ask) remains. RAM use 35.4 % → 19.6 %, back to 0.18.x levels. The wider-timespan goal moves to a new change for exploring how to fit it.
- 0.19.5: Smoothed scroll via time-driven virtual head. The decoder fills the ring in ~8-col bursts at 1 s timespan, then waits ~35 ms for the next drain — visible as irregular scrolling. Added a monotonic `abs_head` counter to the waveform ring (never wraps) and a `g_waveform_virt_abs` in main.cpp that advances by elapsed-µs × sample-rate / samples-per-col each tick, capped at `abs_head - 12 cols` so it lags actual production by a small jitter buffer. The wave composition uses the smoothed virtual head as the wrap point, giving a perceptually constant scroll speed. Pauses skip the advance; ring resets re-sync the anchor.
- 0.19.4: pollWaveform tick raised 16 ms → 30 ms and the "head hasn't changed, skip" short-circuit removed. Didn't fix smoothness on its own — kept anyway because each frame now genuinely shows current state.
- 0.19.3: Ctrl+W toggle was dismissing on every ctrl event including the release of W (which fires `isChange` with `state.ctrl=true` and empty `word`). Tightened the waveform-branch ctrl handler to require an actual W in `state.word` before dismissing, matching the global Ctrl+W handler.
- 0.19.2: layout flip changes RAM math. With 80 % future, the future region needs to span 80 % of timespan in actual samples held in the pre-buffer. To support 1 s timespan (800 ms future, ~730 ms pre-buffer), `PREBUF_SAMPLES` raised 16 000 → 33 000 (~32 KB → ~66 KB). 2 s dropped — would have needed ~135 KB pre-buffer alone. Timespan options now `0.5s` and `1s` only. RAM use 25.1 % → 35.4 %.

### Pass-through keys

Keys that act on playback or device feel — and therefore make sense to use *while watching* the waveform — fire their normal action without dismissing the overlay:

- `space` — pause / resume
- `{` / `}` — prev / next track
- `'` — jump to playing
- `[` / `]` — seek (held)
- `1`–`0` — jump to %
- `-` / `=` — volume
- `Fn+=` / `Fn+-` (and shifted) — brightness

### Dismissal keys

Everything else continues to dismiss — the user is leaving waveform mode either to navigate, search, or do something structural:

- Any letter — opens fuzzy search after dismissal
- `Ctrl+W` — explicit waveform toggle off
- `Fn+\`` — esc
- `?` — opens Settings
- `Del` — opens reset modal
- `enter`, `;`, `.`, `,`, `/`, `+`, `_`, `` ` ``, `\`, `~` — navigation / structural / view-config keys with no meaning in waveform mode

### Volume / pause redraws under the overlay

`changeVolume` and `togglePause` skip `drawFooter()` when an overlay is active. The waveform overlay paints over the footer area, so today they do the right thing (no redraw underneath). Need to verify that the volume key auto-repeat (`pollVolumeKeys`) and the existing `togglePause` path both leave the waveform composition untouched.

The per-tick `pollWaveform` already redraws the canvas at ~60 Hz while the overlay is up, so the next tick repaints whatever the action mutated underneath. Nothing extra needed.

### Implementation shape

Rewrite the waveform branch in the input dispatcher:

- For each pass-through key, call its action handler directly.
- For each dismissal key (anything else meaningful), set `g_waveform_active = false` and call `draw()`.
- Keys with no meaning anywhere (currently `state.tab` was already a dismissal trigger) become no-ops rather than dismissals.

`composeWaveformView` widens its drawing rectangle from `(by, bh)` to `(0, SCREEN_H)`. The playhead inset, ring-buffer lookup, and column count are unaffected — only the vertical span changes.


