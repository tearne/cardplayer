# Waveform letter dismiss

**Mode:** Wander

## Intent

When the waveform view is active, pressing a letter key currently dismisses the waveform *and* immediately enters search mode (with that letter as the query seed). Split those into two actions: first letter dismisses the waveform; the next letter press enters search. Reduces the chance of accidentally starting a search when the user just wanted to exit the waveform view.

## Conclusion

Generalised the initial idea after the first attempt: instead of just letters-in-the-browser case, the waveform overlay now consumes *any* meaningful keypress to dismiss itself, regardless of what's underneath (search, browser, etc.). The next press is interpreted normally. Fixes the case where pressing a letter while a waveform was overlaid on an active search silently appended to the query rather than dismissing the overlay.

**Changelog entry:**

> Waveform overlay dismissal is now consistent: any keypress dismisses it and is consumed; the next press acts normally in the mode underneath. Previously, pressing a letter while the waveform was overlaid on a search would silently append to the query instead of dismissing the overlay.
