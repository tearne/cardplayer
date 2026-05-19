# Panel tearing investigation

**Mode:** TBD

## Intent

The ST7789 panel has no TE (tearing-effect) signal exposed, so push timing can't be synchronised to its internal scan. During heatmap pushes and diagnostics toggles, tearing manifests as a diagonal boundary — characteristic of the DMA write direction crossing the panel's refresh direction at different rates.

Two angles worth exploring: (a) raising SPI clock so a push finishes within one panel scan period (tear stays consistently trailing the scan beam), (b) measuring scan period empirically and inserting a phase-aligning delay between `waitDisplay()` return and the next push, so the write lands in the panel's blanking window.

Parked until prioritised against other work.
