# Windowed-peak detection for the limiter

## Intent

The leveller's limiter (reworked in `leveller-distortion`) detects peaks from the single incoming sample and relies on a fast attack plus slow release to catch them within the lookahead. That works, but it leaves two footguns the user can hit with the exposed knobs: if attack is slower than the lookahead, peaks slip through and the ceiling clamp hard-clips them; and a very fast attack can modulate the gain within a low-frequency cycle, adding its own buzz.

Replacing instantaneous detection with **windowed-peak detection** — making the gain target the lowest gain any sample across the lookahead window will need (a sliding-window minimum) — would remove both. Because the target already accounts for the loudest upcoming sample, the clamp never fires regardless of attack, and attack stops being a clip-safety setting and becomes purely a smoothness control. The clean, distortion-free zone then spans essentially the whole knob range rather than a careful band.

Deferred deliberately: the simpler fast-attack limiter likely sounds clean at sane settings, so this is the upgrade to adopt only if tuning shows the clean-and-smooth zone is too narrow to live in. Modest cost (a sliding-window-minimum over the delay line — a few KB of RAM and a small amount of per-sample work).
