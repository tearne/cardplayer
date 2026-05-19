# Waveform abs_head cleanup

**Mode:** Wander

## Intent

Remove the `WaveformRing.abs_head` field and its increment. It was added by the 0.19.9 wider-waveform prototype as a monotonic cursor for a viewport approach later replaced by the shared `g_viz_disp_abs`. The increment now writes a counter that nothing reads.

## Conclusion

Field removed from `WaveformRing`, increment dropped from `ConsumeSample`. No callers existed for `_wv.abs_head` — grep confirmed before deletion, build verified after. No user-visible change; not changelog-worthy.
