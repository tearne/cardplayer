# Settings screen

## Intent

The user-tunable behaviours — name wrapping, diagnostics visibility, font size, and others as they appear — are currently invisible until you've read the help screen, and even then only the keybind is shown, not the current state. A new screen lets the user discover what's adjustable, see each setting's current value, learn the key that toggles it, and change it from within the screen itself. Discoverability and self-documentation, with the keybind shortcuts continuing to work as they do now.

This change is closely related to `persisted-state`: the same set of toggles is the natural target for both surfacing (here) and persistence (there). Build order is worth thinking through — whichever lands first informs the other's surface area.
