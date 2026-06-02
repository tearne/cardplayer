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

Expose the knobs as ordinary Settings rows next to the volume rows — a toggle plus numeric rows using the existing `,`/`/` adjust idiom. *Reason: matches the established Settings pattern; few enough rows that no sub-screen is warranted.*

### On/off control

A dedicated "Leveling" toggle row (drive gain and release are remembered while off), plus a global `Ctrl+L` shortcut, plus a small on-screen indicator shown only while leveling is active. *Reason: a hardware-fast toggle for the thing you flick per-book, with a persistent visual cue so the on/off state is never ambiguous. `Ctrl+L` is free as of planning (current Ctrl bindings: W/S/T/H/D) — re-confirm it's still free at build time, since other in-flight changes may claim keys first.* Indicator placement is a minor build-time detail — likely a small glyph at the far-left of the footer, since the volume-bar-adjacent space is taken by the volume integer overlay.

### Lock-free parameter handoff

The limiter's state (envelope, delay line) lives in the audio-output object, touched only by the audio task. UI edits write plain scalars/precomputed coefficients into it via setters; the audio task reads them per-sample without a lock. *Reason: no mutex in the per-sample hot path; a torn scalar read is harmless and self-corrects on the next sample.*

### Latency and memory

The lookahead delay line adds a few ms of latency (negligible against the chain's existing ~220 ms decode-ahead) and under 1 KB of static RAM. It must be cleared in the `hardFlush` path alongside the pre-buffer, so stop/skip/seek stay instant and don't replay stale lookahead samples. *Reason: lookahead's only cost is a short delay and a small buffer; the only correctness requirement is flushing it with everything else.*

### Persistence and default

New keys in the existing `player` NVS namespace, saved through the existing dirty/flush coalesce and cleared by Reset-all. Leveling defaults **off**. *Reason: opt-in — existing playback is unchanged until the user enables it.*

## Plan

- [ ] Add a leveling processor to the audio-output object: per-sample drive gain → lookahead brickwall limiter with slow release, using a small internal delay line.
- [ ] Apply it in `ConsumeSample` after the mono down-mix, ahead of the visualisation taps and the pre-buffer store.
- [ ] Clear the lookahead delay line in `hardFlush`.
- [ ] Lock-free parameter setters (enabled, drive gain, release); precompute limiter coefficients when a value changes.
- [ ] Global leveling state + NVS load/save + Reset-all defaults (off, +6 dB, 0.5 s).
- [ ] Settings rows: "Leveling" toggle, "Drive gain" (dB), "Release" (s) — value display and `,`/`/` adjust within range.
- [ ] `Ctrl+L` global toggle (confirm the binding is still free).
- [ ] Footer indicator shown only while leveling is active.
- [ ] Create a follow-on change for the per-alarm limiter override (see Asides).

## Asides

- **Gain-reduction visualiser** (longer-term). A meter or overlay showing how hard the limiter is working on the current section of audio, so the user can see it acting and tune the settings against real material. Deferred from this change.

- **Per-alarm limiter override** (follow-on change, spun up by the final Plan task). Motivation: fall asleep to an audiobook with leveling on, wake to music with it off. Each alarm carries a tri-state field — **leave / force on / force off** (default *leave*, so no behaviour change) — applied to the limiter at fire time via its on/off setter. Adds an alarm-editor row and one NVS key per slot (e.g. `aN_l`). Restore-on-dismiss behaviour follows alarm-clock's "snapshot prior state on fire" decision: if the alarm restores prior volume on dismiss, it restores the prior limiter state too; otherwise the alarm's choice simply persists. Build only once both the limiter and the alarm system exist.
