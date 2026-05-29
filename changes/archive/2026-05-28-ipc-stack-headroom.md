# IPC stack headroom

**Mode:** Formal

## Intent

The ESP-IDF per-core IPC tasks (`ipc0`/`ipc1`) run system code we don't control on a fixed-size stack. The stack peak has crept up from ~1088 bytes (May 8) to over 2048 (today), tripping the canary during GPIO ISR install at boot and producing intermittent boot-loops. We can't predict the worst-case analytically — the work happens inside the system allocator, against a heap layout that drifts with every static-RAM change we make.

The current path of "bump when it breaks" is unsustainable because each surprise is a hard-to-diagnose boot-loop on hardware. Two pieces of work make it sustainable:

1. **Bump the stack to 4 KB** for immediate margin. Cheap (~2 KB on a 512 KB chip).
2. **Add a probe** that reports `ipc0`/`ipc1` peak usage so future creep is visible early — before it becomes a boot failure.

This unblocks the chess-difficulty build, which is currently paused with its code uncommitted in the working tree.

## Approach

### Bump target: 4096

Doubling the current 2048. Cheap on a 512 KB chip and gives us a 2 KB cushion above any peak we've ever observed. 8 KB was on the table but is needless until we have evidence the peak is still climbing — which the probe will give us.

### Where the value lives

Both `platformio.ini` (`custom_sdkconfig` block) and `sdkconfig.defaults` carry `CONFIG_ESP_IPC_TASK_STACK_SIZE=2048`. Update both to 4096 and keep them in sync — the prior change established that `custom_sdkconfig` does propagate, but both files are read by the framework rebuild and divergence is a foot-gun for future archaeology. The explanatory comment above the line in `platformio.ini` is updated to reflect the new numbers and the fact that the peak is now monitored rather than guessed.

### Probe

A single boot-time diagnostic: read `uxTaskGetStackHighWaterMark` for the `ipc0` and `ipc1` task handles (obtained via `xTaskGetHandle("ipc0")` / `xTaskGetHandle("ipc1")`) and print a one-line summary to `Serial`. The FreeRTOS high-water mark is the *minimum free* seen since the task started — it's monotonic, so a single read late in `setup()` captures the worst case from boot. No need for periodic sampling.

Output shape: `ipc0 min_free=NNNN ipc1 min_free=NNNN` on one line, prefixed with a tag (e.g. `[ipc]`) so it's grep-friendly on serial.

Surface: serial *and* the diag overlay. The overlay row currently displaying the per-core CPU numerics (now redundant with the CPU graph) is repurposed to show the two `ipc{0,1}` min-free values. Visible on-device without a USB cable.

### Version

This change ships 0.22.7 → 0.22.8. The 0.22.7 label currently sits in the uncommitted chess working tree but never produced a bootable binary; 0.22.8 is the first binary that actually flashes and runs. Chess-difficulty's Conclusion will retroactively note that its shipped version was 0.22.8, folded together with the IPC fix at flash time.

### Build lock

`active.md` currently names `chess-difficulty.md`. On Build entry for this change, swap it to `ipc-stack-headroom.md`. On Conclusion here, swap it back to `chess-difficulty.md` so the paused chess build can resume verification on the new firmware.

### Out of scope

- Generalising the probe to all FreeRTOS tasks.
- Engineering a tighter fit for the IPC stack (the analysis above established this isn't viable).
- Tuning other system task stacks.
- Map updates — no node is disturbed.

## Plan

- [x] Bump `CONFIG_ESP_IPC_TASK_STACK_SIZE` to 4096 in `sdkconfig.defaults` and `platformio.ini`, updating the explanatory comment
- [x] Add an `[ipc]` line on `Serial` at end of `setup()` reading `uxTaskGetStackHighWaterMark` for `ipc0` and `ipc1`
- [x] Repurpose the per-core CPU numerics row on the diag overlay to display `ipc0` / `ipc1` min-free
- [x] Bump version 0.22.7 → 0.22.8

## Log

- Probe stores task handles once at end of `setup()` (after the boot peak has occurred), then `pollDiagnostics` re-reads `uxTaskGetStackHighWaterMark` on each pass. The high-water mark is monotonic, so the on-screen `i0:`/`i1:` values update only when a new worst case is seen.
- Kept the existing per-core tint colours (cyan for core 0, orange for core 1) on the new `i0:`/`i1:` rows so the visual association with cores is preserved.
- Removed `g_diag_cpu0_pct` / `g_diag_cpu1_pct` entirely — the per-core CPU sparkline carries the load info; the text mirror is gone.
- On-device readings at 4096: `ipc0 min_free=3324` (used 772 B), `ipc1 min_free=3016` (used 1080 B). The ipc1 peak is essentially identical to the May 8 reading (1088 B).
- **Deterministic per-binary peak.** User power-cycled multiple times; `i0` / `i1` on the overlay are identical every boot. Refines the prior "stochastic worst-case" theory: for a given binary the boot path through GPIO ISR install lands at the same allocator walk depth every time, so the peak is reproducible. What was non-deterministic was *not* peak usage — it was the relationship between peak usage and the canary slot at the cliff edge.
- **The IPC stack size is self-referential.** Bumping the stack changes the heap layout (the two IPC stacks now occupy 4 KB more), which changes the free-list state at the moment of boot-time alloc, which changes the walk depth. So we cannot infer what the peak was at 2048 from the 1080 measurement at 4096 — they are measurements of different systems. The peak at 2048 with this same code might have been anywhere in the 2000–2050 range (enough to trip the canary on every boot of 0.22.7, occasionally on 0.22.6 with slightly different layout).
- **Implication for sustainability.** Per-binary peak is reproducible, so the probe gives reliable per-build telemetry — once we read `i0`/`i1` on first boot after a release, that's the number for that binary. Future binaries may differ because any static-RAM change perturbs heap layout. Headroom at 4096 against current peak: ~3 KB on ipc1, ~3.3 KB on ipc0. Solid.

## Conclusion

Stack bumped to 4096, probe in place on serial and on the diag overlay (right column, where the redundant per-core CPU numerics were). On-device peak readings at 4096: ipc1 = 1080 B, ipc0 = 772 B; both deterministic across power cycles.

The investigation refined the mental model in a useful way: the IPC peak is *deterministic per-binary*, not stochastic. What was intermittent in 0.22.6 wasn't the peak — it was whether the canary byte specifically got stomped at the cliff edge. Bumping the stack also reshapes the heap layout, so the 1080 reading is the new system's peak, not the old system's. Future creep will be visible on the overlay before it becomes a boot failure.

The 0.22.8 binary tested includes this change plus chess-difficulty's pending tree changes. The chess-difficulty Conclusion will fold its work into the same 0.22.8 changelog entry rather than bumping the version further.

### Proposed changelog entry

```
## 0.22.8 — 2026-05-28

- Internal: ESP-IDF IPC task stacks bumped from 2 KB to 4 KB to clear an intermittent boot-time canary panic on ipc1. The previous 2 KB sizing was sitting right at the cliff edge with no visibility; a new probe in `setup()` now reads `uxTaskGetStackHighWaterMark` for `ipc0` / `ipc1` and surfaces the peak both on serial (`[ipc] ipc0 min_free=... ipc1 min_free=...`) and on the diagnostics overlay (replacing the per-core CPU text, redundant with the CPU sparkline). Boot peak is deterministic per binary; current ipc1 peak ~1 KB used, leaving ~3 KB headroom.
```

