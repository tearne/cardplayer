# Loudness limiter

**Mode:** Formal

## Intent

Audiobooks often swing between near-inaudible passages and jarringly loud ones (whispers vs shouts), making them hard to listen to at a fixed volume in a quiet room. Pre-processing every file through a compressor offline is tedious. The user wants the player to even out loudness on the fly during playback — lifting quiet passages and taming loud ones — adjustable live, persisted across reboots, and requiring no change to the source files.

The chosen mechanism (to settle in Approach) is the "drive-into-a-limiter" workflow the user already knows: input gain pushes quiet material up while a lookahead brickwall limiter catches the loud peaks at a ceiling, with a slow release for smoothness.

## Approach

### Insertion point

Process each sample inside `ConsumeSample`, immediately after the stereo→mono down-mix and *before* the waveform/spectrum taps and the pre-buffer store. *Reason: one mono channel to process; the visualisations then show what you actually hear; and it sits before the codec's volume control, so leveling is independent of the user's volume knob.*

### Processing chain

Static input (drive) gain → lookahead brickwall limiter with a slow release. The lookahead is a short internal delay line (a few ms), fixed and not user-exposed. *Reason: the "drive-into-a-limiter" workflow — fixed gain lifts quiet passages, the ceiling tames loud peaks, the slow release avoids pumping. No adaptive AGC, so it stays one simple stage; a small fixed lookahead is ample for clean peak-catching on speech without adding a knob.*

### Limiter internals

Process in float. Apply drive gain, then derive the needed gain reduction from the peak over the lookahead window (`reduction = ceiling / peak` when `peak > ceiling`, else none), smoothing toward it so the reduction lands as the peak arrives and recovers over the release time; apply that gain to the *delayed* sample and convert back to int16 clamped at the ceiling. *Reason: float headroom prevents wrap-around when drive pushes a sample past full-scale before the limiter acts; pinning the standard lookahead structure stops it being reinvented or built naively.*

### Parameter ranges and defaults

Gain and ceiling in dB, release as a time — matching how the user reasons about compression.


| Parameter | Exposed | Range | Default |
|---|---|---|---|
| Leveling on/off | toggle + `Ctrl+L` | — | off |
| Drive gain | numeric (dB) | 0…+24 dB | +6 dB |
| Release | numeric (time) | 0.1–2 s | 0.5 s |
| Ceiling | fixed | — | ~−1 dBFS |
| Lookahead | fixed | — | a few ms |

### Settings placement

A "Leveling..." action row in Settings (after "Volume max") opens a dedicated Leveling sub-screen — mirroring the Alarms sub-menu — holding the Leveling toggle, Drive gain, Release, a **Reset to default** action, and Back. *Reason: a Reset-to-default action wants to sit with the knobs it resets, which tips the cluster over into its own screen rather than flat Settings rows.* Reset-to-default restores only the tuning (Drive +6 dB, Release 0.5 s) and leaves the on/off state alone — a user in this screen is re-tuning, not disarming.

### On/off control

A dedicated "Leveling" toggle row (drive gain and release are remembered while off), plus a global `Ctrl+L` shortcut, plus a persistent on-screen cue while leveling is active. *Reason: a hardware-fast toggle for the thing you flick per-book, with a visual cue so the on/off state is never ambiguous. `Ctrl+L` is free as of planning (current Ctrl bindings: W/S/T/H/D) — re-confirm it's still free at build time, since other in-flight changes may claim keys first.* The cue is the footer **progress bar tinted amber** (`COL_LEVEL_ACCENT`) during playback while leveling is on, falling back to the normal slate-blue (playing) / grey (paused/stopped) otherwise — so it trades the playing colour for the leveling cue only while playing.

### Lock-free parameter handoff

The limiter's state (envelope, delay line) lives in the audio-output object, touched only by the audio task. UI edits write plain scalars/precomputed coefficients into it via setters; the audio task reads them per-sample without a lock. *Reason: no mutex in the per-sample hot path; a torn scalar read is harmless and self-corrects on the next sample.*

### Latency and memory

The lookahead delay line adds a few ms of latency (negligible against the chain's existing ~220 ms decode-ahead) and under 1 KB of static RAM. It must be cleared in the `hardFlush` path alongside the pre-buffer, so stop/skip/seek stay instant and don't replay stale lookahead samples. *Reason: lookahead's only cost is a short delay and a small buffer; the only correctness requirement is flushing it with everything else.*

### Amplification trace

When leveling is on, the waveform visualisation carries a scrolling line across its upper half whose height tracks **net amplification** — the total gain applied vs the raw audio (drive combined with the live limiting), mapped 0 dB at centre to +24 dB at the top. *Reason: net amplification shows both halves of the leveling — riding high while quiet passages are lifted, dipping as loud peaks are tamed — whereas a pure gain-reduction meter only shows the taming and sits dead during the lift, which is most of an audiobook. Tied to the waveform (not the spectrum) so the trace aligns with the audio you're watching; only shown while leveling is on.* The per-column value is the lowest net gain in that column, so a tamed transient registers as a visible dip. Same accent colour as the footer "L".

### Persistence and default

New keys in the existing `player` NVS namespace, saved through the existing dirty/flush coalesce and cleared by Reset-all. Leveling defaults **off**. *Reason: opt-in — existing playback is unchanged until the user enables it.*

## Plan

- [x] Add a leveling processor to the audio-output object: per-sample drive gain → lookahead brickwall limiter with slow release, using a small internal delay line.
- [x] Apply it in `ConsumeSample` after the mono down-mix, ahead of the visualisation taps and the pre-buffer store.
- [x] Clear the lookahead delay line in `hardFlush`.
- [x] Lock-free parameter setters (enabled, drive gain, release); precompute limiter coefficients when a value changes.
- [x] Global leveling state + NVS load/save + Reset-all defaults (off, +6 dB, 0.5 s).
- [x] Leveling sub-screen (off the "Leveling..." Settings row): Leveling toggle, "Drive gain" (dB), "Release" (s), "Reset to default" (tuning only), Back — value display and `,`/`/` adjust within range.
- [x] `Ctrl+L` global toggle (confirm the binding is still free).
- [x] Footer cue while leveling is active (progress bar tinted amber during playback).
- [x] Net-amplification trace across the top of the waveform while leveling is on (per-column min net gain, 0…+24 dB).
- [x] Create a follow-on change for the per-alarm limiter override (see Asides).

## Asides

- **Gain-reduction visualiser** — *folded into this change* (see the Amplification trace Approach decision and Plan task). Realised as a net-amplification trace rather than a pure gain-reduction meter, because gain reduction alone misses the lift half of the leveling.

- **Per-alarm limiter override** (follow-on change, spun up by the final Plan task). Motivation: fall asleep to an audiobook with leveling on, wake to music with it off. Each alarm carries a tri-state field — **leave / force on / force off** (default *leave*, so no behaviour change) — applied to the limiter at fire time via its on/off setter. Adds an alarm-editor row and one NVS key per slot (e.g. `aN_l`). Restore-on-dismiss behaviour follows alarm-clock's "snapshot prior state on fire" decision: if the alarm restores prior volume on dismiss, it restores the prior limiter state too; otherwise the alarm's choice simply persists. Build only once both the limiter and the alarm system exist.

## Log

- `Ctrl+L` confirmed free at build time; current Main-screen Ctrl bindings are W/S/T/H/D and `?`. Placed `Ctrl+L` in that same Main-screen mode-switch block alongside its siblings — so like W/S/T/H/D it acts from the Main screen (browser / search / viz), not from chess / clock / modals. The footer indicator is Main-only anyway (footer is hidden under overlays), so this keeps toggle and visual cue together. Flag for the user if truly global scope was wanted.
- The limiter lives in its own module `src/loudness_limiter.h` (domain concept), included by `audio_output_m5.h`. Sample-rate-dependent coefficients (lookahead length, attack, release) recompute on format change via `SetChannels`, so release time stays correct across 44.1/48 kHz streams.
- Footer indicator: a 7 px-wide "L" stolen from the front of the name slot; the track-name marquee indents by the same width while leveling is on. A leveling toggle doesn't recompute the marquee extent until the next track change, so a long name's scroll range is off by 7 px until then — sub-visible, left as-is.
- Drive gain steps in whole dB (0…+24), release in 0.1 s steps (0.1…2.0 s) via the `,`/`/` idiom.
- Built clean for `cardplayer`: RAM 30.7%, flash 28.2%.
- Post-review change (user-requested): leveling moved from three flat Settings rows into a dedicated "Leveling..." sub-screen (mirrors the Alarms sub-menu) gaining a "Reset to default" action that restores tuning only (Drive +6 dB, Release 0.5 s) and leaves on/off alone. This reverses the Approach's original "no sub-screen warranted" decision; Approach and Plan updated to match. Footer "L" and `Ctrl+L` unchanged. Rebuilt clean; version bumped 0.25.0 → 0.25.1.
- Post-review change (user-requested): folded in the deferred gain-reduction-visualiser aside as a net-amplification trace over the waveform's upper half. Limiter exposes `netGain()`; the waveform ring carries a parallel per-column `amp[]` byte (min net gain, 0…+24 dB); `drawWaveformColIntoCanvas` draws a per-column segment bridging to the previous column so it scrolls as a continuous line. `Ctrl+L` sets the viz dirty flag so the trace appears/clears immediately. Version bumped 0.25.1 → 0.25.2.
- Post-review tweak (user-requested): the trace/"L" were a muted teal (`COL_FOOTER_VOL`) that washed out against both the slate-blue waveform and black. Gave leveling a dedicated accent `COL_LEVEL_ACCENT` (amber-orange `0xFD20`) used by both cues — warm so it contrasts with the cool waveform and stays bright on black. Version bumped 0.25.2 → 0.25.3.
- Post-review tweak (user-requested): dropped the footer "L" glyph (and its name-slot indent) in favour of tinting the footer progress bar amber while leveling is on during playback. Trade-off: while leveling is on and playing, the bar no longer shows the slate-blue playing colour (it's amber); paused/stopped stays grey, so the leveling cue isn't shown while paused. Version bumped 0.25.3 → 0.25.4.

## Conclusion

Built to plan, plus four post-review additions folded in on request (all logged): the leveling controls moved into their own Settings sub-screen with a Reset-to-default (tuning only); a net-amplification trace was added over the waveform; the cues gained a dedicated amber accent; and the on/off cue became an amber progress-bar tint rather than a glyph. `Ctrl+L` is scoped to the Main screen alongside its W/S/T/H/D siblings, not global across chess/clock/modals — left as-is pending a preference. Shipped 0.25.4; not yet verified on hardware.

Documentation impact: leveling is a new playback concept the map doesn't yet cover — needs a node, handled as a follow-up per-node negotiation. The per-alarm limiter override is spun off as `per-alarm-limiter-override.md`.
