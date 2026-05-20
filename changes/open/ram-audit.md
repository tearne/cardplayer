# RAM audit and reduction

**Mode:** Explore

## Intent

Free heap is reading 93% in use at runtime — uncomfortably close to exhaustion. Recent visualisation work (viz_sprite, ring buffers, FFT) and earlier additions (fuzzy index, M4A demuxer, lots of canvas-based UI) have piled up.

Audit where the RAM is actually going, then propose reductions that don't sacrifice user-facing features. Targeted savings (e.g. trimming an oversized buffer, halving a sprite's colour depth, dropping a debug-only piece) are ideal; deeper refactors only if the easy wins don't get us comfortable headroom.

## Approach

### Largest known allocations (mapped from source)

| Buffer | Size | Source |
|---|---|---|
| `g_canvas` (8 bpp) | 32.4 KB | full-screen back buffer (240 × 135) |
| `g_viz_sprite` (8 bpp) | 27.4 KB | viz back buffer (240 × 114) |
| `_prebuf` | 14.0 KB | audio pre-buffer (7000 × int16) |
| `_spec.intensity` | 13.4 KB | spectrum ring (480 × 28 uint8) |
| `_buf[3]` | 9.2 KB | speaker DMA staging (3 × 1536 × int16) |
| Audio task stack | 8.0 KB | `xTaskCreatePinnedToCore` |
| Viz task stack | 4.0 KB | `xTaskCreatePinnedToCore` |
| ArduinoFFT buffers | 3.0 KB | 3 × 256-sample float arrays |
| `_wv` (min + max) | 1.0 KB | waveform ring (480 × 2 int8) |

Total of these alone: ~113 KB.

### Reduction candidates, tiered by risk

**Tier 1: low risk, easy wins**

- `SPEC_COLS` 480 → 256. The minimum is 241 (display width + 1), so 256 gives 16 cols (~270 ms) of safety against any edge-case lag. The 480 figure was over-cautious from the original debugging. Saves ~6.3 KB.
- `WV_COLS` 480 → 256. Same rationale. Saves ~0.5 KB.
- `_buf` `RING_COUNT` 3 → 2. The third slot was a margin; the speaker queue itself is 2-deep so two staging slots suffice. Saves ~3.1 KB. Risk: marginally tighter on underrun headroom.
- Audio task stack 8 KB → 6 KB. Diagnostics already shows peak usage; should be well under 6 KB. Saves ~2.0 KB.
- Viz task stack 4 KB → 3 KB. Render task is straightforward; should fit. Saves ~1.0 KB.

Tier 1 sub-total: **~12.9 KB**.

**Tier 2: moderate impact**

- `viz_sprite` 8 bpp → 4 bpp. 16 colours instead of 256. The viridis ramp already quantises hard at 8 bpp (RGB332 has only 4 blue levels), so visual loss may be small. Need to verify push performance and viridis-LUT visual quality. Saves ~13.7 KB.
- Pre-buffer 7000 → 5000 samples. Trims look-ahead from ~158 ms to ~113 ms. Need to verify no new underruns. Saves ~4.0 KB.

Tier 2 sub-total: **~17 KB**.

**Tier 3: more invasive**

- Replace `g_canvas` with row-strip rendering for header / footer / browser separately. Would save ~25 KB but is a substantial architecture change.

### Strategy

Start with Tier 1 (cheap, low risk) and measure free heap. If we're comfortably above 80 % free of starting headroom, stop there. Otherwise add Tier 2 items one at a time. Tier 3 only if absolutely necessary.