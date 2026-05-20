# Viz renders into canvas directly

**Mode:** Formal

## Intent

Bug: at ~93 % RAM use, toggling diagnostics off (sprite resize from 19 KB to 27 KB) can fail in `createSprite` due to heap fragmentation. The viz sprite is left with a null buffer; subsequent renders re-enter the failing recreate path every 17 ms → blank screen + 100 % CPU on the viz core.

Fix is to eliminate `g_viz_sprite` entirely and render the visualisation directly into the existing `g_canvas`'s browser area. The sprite was a 27 KB duplicate of pixels that already live in the canvas — its only purpose was to enable scroll-and-draw independent of canvas state. We can do the same scroll directly on the canvas browser region via `copyRect` (which handles overlapping memmove correctly).

Net effect: removes the failure mode AND saves ~27 KB of RAM (more than the entire Tier 1 RAM-audit budget).

## Approach

### Replace `g_viz_sprite` with direct canvas rendering

`drawHeatmapColIntoSprite`, `drawWaveformColIntoSprite`, `drawVizColIntoSprite` switch from writing to `g_viz_sprite` to writing into `g_canvas` at the appropriate Y offset (browser-inner). The Y region is exactly what `g_viz_sprite` used to occupy; only the target changes.

### Scroll via `g_canvas.copyRect`

The per-render scroll becomes `g_canvas.copyRect(0, browserY()+1, SCREEN_W - VIZ_COLS_PER_PUSH, inner_h, VIZ_COLS_PER_PUSH, browserY()+1)` — shifts the browser-inner block left by N. LGFX's copyRect handles the overlap correctly. The rightmost N cols are then redrawn.

### Mutex consolidation

`g_viz_sprite_mutex` retires. Canvas writes from `vizTask` and from the main loop's `drawHeader` / `drawFooter` etc. could otherwise race a concurrent `presentFrame`. Extend `g_display_mutex` to wrap canvas-write blocks in `composeVisualisationOverlay` and `pollVisualisation` (not just the pushes). Main-loop callers that write the header / footer regions and follow with a present already serialise via `display_mutex` in the present step; viz-region writes don't conflict with header-region writes (distinct memory), so additional locking on those is unnecessary.

### `initVizSprite` retires

Becomes a no-op. The canvas is already allocated once at boot at `SCREEN_W × SCREEN_H` and never resized.

## Plan

- [x] Strip `g_viz_sprite`, `g_viz_sprite_h`, `g_viz_sprite_mutex`, `initVizSprite`, `fullRedrawVizSprite` from `main.cpp`. (`g_viz_sprite_dirty` retained as the force-full-redraw flag.)
- [x] Refactor `drawHeatmapColIntoSprite` → `drawHeatmapColIntoCanvas` (and waveform / `drawVizColIntoSprite` likewise) to write into `g_canvas` at the canvas-relative Y position.
- [x] Refactor `composeVisualisationOverlay` and `pollVisualisation` to scroll via `g_canvas.copyRect` and call the new per-column draw functions.
- [x] Replace `g_viz_sprite_mutex` usage with `g_display_mutex` around the canvas-write block in `pollVisualisation` (release before `presentRows` to avoid re-entry).
- [x] Remove the (now unused) `g_viz_sprite_mutex` global and its creation in `setup`.
- [x] Verify build, flash, smoke-test viz + diag toggle. RAM 93 % → 86 % (~22 KB freed).

## Conclusion

Completed. The viz pipeline now renders straight into `g_canvas` instead of going via a dedicated 8 bpp sprite that mirrored the same pixels. Scroll uses `g_canvas.copyRect` on the browser-inner region (LGFX handles the memmove overlap). All canvas-touching paths funnel through `g_display_mutex` (released before `presentRows`, which re-acquires it for the push). The 93 → 86 % RAM drop confirms the saving.

The original bug (blank screen + 100 % CPU at high heap pressure) is gone by construction — there's no longer a sprite to fail to reallocate.

**Documentation impact:** none — pure internal refactor.
