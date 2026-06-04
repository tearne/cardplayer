# Header and menu cosmetics

**Mode:** Wander

## Intent

Two small cosmetic tidy-ups, done together:

- **Drop the Back rows from menus.** Leveling, Alarms, Alarm Editor and Set Current Time each end with an explicit "Back" row that's now redundant — Esc (`` ` ``) backs out of every screen and Del backs out of menus. Remove them so the menus are shorter and back-out is the single standardised Esc.

- **Brighten the header battery voltage.** The voltage readout (just left of the path breadcrumb) is a dim grey (`COL_HEADER_TXT` `0x7BEF`) and hard to read. Make it clearer without it blurring into the white path name to its right — the path is white (`0xFFFF`), so the voltage needs either a distinct brighter colour or a separator (or both) rather than simply going white too.

## Log

- Battery voltage recoloured to bright cyan (`COL_BATTERY_VOLT` `0x07FF`) — bright and a distinct hue from the white path breadcrumb, so the two don't merge. No layout change.
- Dropped the `Back` row from all four menus (Leveling, Alarms, Alarm Editor, Set Current Time) — removed each enum entry + its label/value/activate handling. Exit is now Esc (`` ` ``, via backOut) or Del in every case, both already wired. `Commit` stays on Set Time.
- Build catch: removing `SCT_BACK` left `SCT_COMMIT` falling through to a now-deleted body in the value switch — gave it its own empty-value return. Rebuilt clean at 0.26.2.
- Recoloured voltage from cyan to the slate-blue `0x6979` (matches frame separators / progress bar) per user request — 0.26.3. Note: this blue is lower-luminance than the original grey, so it's a palette match but not necessarily brighter; a lighter-but-same-hue blue is the fallback if legibility suffers.
- Brightened the voltage blue to `0x83BF` (lightened slate-blue / cornflower) for legibility, keeping it in the frame/progress blue family — 0.26.4.
- Extended the recolour into a footer palette (user request, 0.26.5): volume bar+number and the idle/paused progress bar now share the blue accent (`COL_ACCENT_BLUE` 0x83BF); the non-leveling playing progress bar is green (`COL_PROG_PLAY` 0x87EF, similar intensity); leveling-on stays amber. Decoupled the waveform from the progress colour — it kept its slate-blue (`COL_WAVEFORM` 0x6979) since the old `COL_FOOTER_PROG` was shared by both. Scope has grown from "battery + back-rows" into a footer palette refresh — worth renaming the change and a map catch-up (Footer node still says "slate-blue playing / grey paused") at conclusion.

## Conclusion

Started as "brighten the battery voltage + drop menu Back rows" and grew, on request, into a small footer colour refresh — renamed accordingly. Final palette: a shared brightened blue (`COL_ACCENT_BLUE` `0x83BF`) for the battery voltage, the volume bar/number, and the idle/paused progress bar; green (`COL_PROG_PLAY` `0x87EF`) for the non-leveling playing progress bar; amber unchanged for leveling-on; the waveform kept its slate-blue (`COL_WAVEFORM`), decoupled from the progress colour the two had shared. Back rows removed from the Leveling, Alarms, Alarm Editor and Set Current Time menus — Esc (`` ` ``) and Del back out. Shipped 0.26.5.

Map catch-up outstanding: the Footer node still describes the old "slate-blue while playing, mid-grey while paused" scheme, and doesn't mention the volume/battery accent — a per-node negotiation for next time the map is touched.
