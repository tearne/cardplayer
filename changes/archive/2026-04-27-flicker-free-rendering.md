# Flicker-free rendering

## Intent

Browser updates visibly flash and tear, most obviously while scrolling quickly through a folder. The user sees a black frame between the old and new content because the column is cleared before being redrawn. Pixels should transition directly from old content to new content, without an intervening blank state.

## Approach

### Off-screen back buffer

A single full-screen `LGFX_Sprite` (240 × 135, 16 bpp) acts as a back buffer. All draw operations target the sprite; once a redraw has finished, the sprite is pushed to the panel in one operation. Pixels on the panel transition directly from the previous frame's content to the new frame's content, with no intervening fill.

### Allocation

The sprite is created once at startup (in `setup()`) and held for the life of the process. Pre-allocation keeps the redraw path free of allocation-failure handling and avoids fragmentation pressure.

The buffer occupies ~65 KB of internal RAM (240 × 135 × 2). The current build reports ~304 KB internal RAM free, so the budget fits comfortably without enabling PSRAM.

### Mechanical conversion

Every existing `drawX()` function follows the same pattern — an `auto& d = M5Cardputer.Display;` alias, optional `startWrite()`/`endWrite()` brackets, then drawing calls. Each function is rewritten so the alias points at the sprite, the explicit transaction brackets are removed (sprite drawing is RAM-only, no SPI transaction to bracket), and a single `pushSprite(0, 0)` at the end of the function flushes the whole frame to the panel. Drawing semantics are unchanged — the sprite presents the same `LovyanGFX` API the display does.

### Push granularity

Every redraw pushes the entire sprite, regardless of which region changed. The push is a single SPI DMA of 64.8 KB (~6 – 7 ms at the panel's clock); the periodic progress-bar updates fire at 500 ms and the held-key cursor at 100 ms, so the push cost is well within those budgets and the simpler model wins over per-region pushes.

### Help overlay and other paths

The help overlay and battery-low warning paths take the same treatment: render into the sprite, then push. Nothing draws directly to the panel.

## Plan

- [x] Declare a global `LGFX_Sprite` (the canvas) and create it at `SCREEN_W × SCREEN_H`, 16 bpp, in `setup()` before the first draw.
- [x] Convert every draw function (`drawHeader`, `drawBrowser`, `drawColumn`, `drawEntry`, `drawScrollbar`, `drawFooter`, `drawSlotProgress`, `drawHelp`, `draw`, plus the battery-low warning render and any full-screen fills) to target the canvas instead of `M5Cardputer.Display`. Remove the now-redundant `startWrite()` / `endWrite()` brackets.
- [x] After each top-level draw function (the entry points that complete a frame — `drawHeader`, `drawBrowser`, `drawFooter`, `drawSlotProgress`, `drawHelp`, `draw`, battery-low warning), push the canvas to the panel with a single `pushSprite(0, 0)`. Internal helpers (`drawColumn`, `drawEntry`, `drawScrollbar`) draw but do not push.
- [x] Bump `APP_VERSION` from `0.11.4` to `0.12.0`.
- [x] On-device test: fast scroll through a long folder no longer flashes; periodic footer progress updates don't flash; help overlay and battery-low warning render cleanly; first paint after boot is intact.

## Log

- Used `M5Canvas` (M5GFX's wrapper around `LGFX_Sprite`) constructed with `&M5Cardputer.Display` as parent; `createSprite(SCREEN_W, SCREEN_H)` runs at the top of `setup()` once the display rotation is set.
- Push model: top-level region functions (`drawHeader`, `drawFooter`, `drawBrowser`, `drawHelp`, `enterBatteryLowState`) push at the end. `draw()` composes those three regions and incurs three pushes per full repaint (~20 ms total at the panel's clock); not flickery — the original black-fill-then-redraw flash inside any single region is gone — but a full repaint shows regions appearing in sequence rather than atomically. Acceptable since `draw()` runs only on big state changes.
- Slot helpers (`drawSlotName`, `drawSlotProgress`, `drawSlotVolume`) don't push; their standalone callers (`pollMarquee`, the per-second progress refresh in `loop()`) call `presentFrame()` themselves so single-region updates remain atomic.

## Conclusion

Completed. The Log captures the push model decisions (full-repaint sequence, single-region atomicity).
