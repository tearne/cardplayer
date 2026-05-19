# Diagnostics toggle in visualisation mode

**Mode:** Formal

## Intent

Currently `` ` `` (the diagnostics-row toggle outside the visualisation overlay) dismisses any active visualisation. That's the wrong default — diagnostics is a natural companion to a running visualisation, not something the user wants to use as an exit shortcut. `` ` `` should pass through to its normal diagnostics-row toggle behaviour while the visualisation is up, leaving the overlay in place.

## Approach

### `` ` `` joins the overlay's pass-through key set

In the visualisation-overlay key dispatch (`g_waveform_active || g_heatmap_active` branch), add `` ` `` to the pass-through cases alongside transport/volume — call `toggleDiagnostics()` and stop. `toggleDiagnostics()` already does `draw()`, which recomposes the browser slot at the new dimensions and dispatches through the active visualisation, so the overlay redraws at the resized height automatically. No need to touch the visualisation polls.

## Plan

- [x] Add `` ` `` to the visualisation-overlay pass-through switch in `main.cpp`; route to `toggleDiagnostics()` instead of falling through to `dismiss = true`.

## Conclusion

Completed.
