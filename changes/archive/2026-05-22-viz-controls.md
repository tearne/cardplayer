# Visualisation Controls

**Mode:** Formal

## Intent

Tighten the visualisation control surface:

- Rename "heatmap" to "spectrum" throughout the UI, settings, and keybinding docs — it's the conventional term for what the view actually shows.
- Rebind the toggle from `Ctrl+H` to `Ctrl+S` to match.
- Rename the "Auto heatmap" setting to "Auto spectrum" for parallel construction with "Auto waveform".
- Add `Tab` as a viz-mode toggle: pressing it during playback restores whichever combination of waveform/spectrum was last shown, and pressing it again dismisses them. Lets the user flick the visualisations in and out without re-selecting which ones.

## Approach

### Rename heatmap → spectrum

The view is a scrolling spectrogram; "spectrum" is the conventional user-facing term and is what the Approach treats as the domain word. Rename consistently down to the source: NVS key, code identifiers (`g_heatmap_active`, `g_auto_heatmap`, `toggleHeatmap`, `drawHeatmapColIntoCanvas`, …), code comments, settings label, and map references. Domain names everywhere ([STYLE.md §3,4](../agent/2026-05-09.1/STYLE.md)).

### NVS key migration

The "Auto heatmap" toggle is stored under `autohm`. Rename to `autosp` with a one-shot migration on boot: if `autosp` is missing and `autohm` is present, copy the value across and remove the old key. Keeps user state across the upgrade without leaving a stale key behind forever.

### Rebind Ctrl+H → Ctrl+S

Two call sites toggle the spectrum overlay: the in-overlay Ctrl-branch (~main.cpp:3211) and the top-level Ctrl-branch (~3287). Both move from `H` to `S`. No new ambiguity — `S` is unused.

### Tab as viz toggle

`Tab` currently dismisses an active overlay (~main.cpp:3216) alongside `del`. Replace with a toggle:

- **Overlay visible:** snapshot the current `(waveform_on, spectrum_on)` pair into `g_last_viz`, then dismiss.
- **Overlay hidden, snapshot present:** restore from snapshot.
- **Overlay hidden, no snapshot:** show both waveform and spectrum — a discoverable first-press reveal.

The snapshot lives in RAM only; it does not persist across power cycles, so a power-on starts clean. The `Auto waveform` / `Auto spectrum` settings continue to drive what opens at each new track unchanged.

Every dismiss path — Tab, Esc, `del`, the "any other key" branch — captures the current `(waveform_on, spectrum_on)` pair into the snapshot before clearing. Tab is the only path that *restores*. So whatever you last had on screen is what a later Tab brings back, regardless of how you dismissed. `Fn+\`` (esc) also continues to exit search and dismiss Settings. Plain `` ` `` (diagnostics row toggle) is untouched in both top-level and viz-overlay contexts.

### Map updates

Map catch-up rides with this change as per-node negotiation after Build. Affected nodes: **Controls** (keybinding list — `Ctrl+S` replaces `Ctrl+H`, new `Tab` entry, `Fn+\`` description updated to drop viz-dismiss), **Settings** ("Auto heatmap" → "Auto spectrum"), **Waveform Visualisation** (likely needs to become a parent or sibling of a new **Spectrum** node — the current map has no Spectrum node), and the **tree overview** at the root.

## Plan

- [x] Rename source identifiers: `g_heatmap_active` → `g_spectrum_active`, `g_auto_heatmap` → `g_auto_spectrum`, `toggleHeatmap` → `toggleSpectrum`, `drawHeatmapColIntoCanvas` → `drawSpectrumColIntoCanvas`, `SR_AUTO_HEAT` → `SR_AUTO_SPEC`
- [x] Update comments in `main.cpp` and `audio_output_m5.h` to use "spectrum" instead of "heatmap"
- [x] Change NVS key `autohm` → `autosp` with one-shot migration on boot (read old, write new, remove old)
- [x] Settings row label "Auto heatmap" → "Auto spectrum"
- [x] Rebind toggle from `Ctrl+H` to `Ctrl+S` in both Ctrl-branches (in-overlay and top-level)
- [x] Add `g_last_viz_waveform` / `g_last_viz_spectrum` snapshot fields
- [x] Replace Tab's current dismiss behaviour with toggle: restore-from-snapshot when hidden, show both when no snapshot
- [x] Make every dismiss path (Tab, Esc, del, any-other-key) capture the snapshot before clearing
- [x] Extend `Fn+\`` dismiss branch to also clear spectrum (currently only clears waveform)
- [x] Map catch-up — per-node negotiation: Controls keybindings, Settings, Waveform Visualisation (split or rename, add Spectrum node), root tree overview
- [x] Changelog entry

## Conclusion

Completed as planned. Map restructure landed (Waveform Visualisation node replaced by Visualisation parent with Waveform + Spectrum children); patch bump 0.21.66 → 0.21.67.

## Log

- Top-level `Fn+\`` (esc) handler had a `g_waveform_active = false` branch that was unreachable: when any viz overlay is on, the in-overlay branch fires first and routes esc into the dismiss path (which snapshots + clears both waveform and spectrum via `snapshotAndDismissViz()`). Removed the dead branch; the top-level esc now only exits search.
- Bumped `APP_VERSION` to `0.21.67`.
- Build clean (both `cardplayer` and `cardputer-fuzzy` environments).
