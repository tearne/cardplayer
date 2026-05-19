# Header rearrange for taller CPU graph

**Mode:** Formal

## Intent

The CPU graph could use ~10 px more vertical space than the header currently gives it. The path/version slot is using that space — but only when CPU is low. Once load spikes, that region of the graph is exactly where the user wants to look.

Swap the header so battery is on the left and version/path on the right. Then extend the CPU graph to the full header height in its right-side region, drawing over the path/version when sparkline pixels overlap. Path stays always visible (no conditional hide); graph wins where they cross.

## Approach

### Swap battery to left, version/path to right

Battery icon at x=0 with voltage label to its right; the version/path slot moves to the right edge, taking the remaining width. Path's static-trim direction stays — leftmost segments drop off when the path is wider than the slot.

### Graph extends to full header height

CPU sparkline's y-range becomes y=0–41 (42 px tall, was 32). X-range unchanged. Bar mapping uses the new height, so the y-resolution improvement is real (42 levels instead of 32).

### Render order: text first, sparkline last

`drawHeader` renders battery + path/version; `drawDiagnosticsRow` renders the sparkline afterwards. Where they overlap (path region under high CPU), sparkline wins. The header's mid-grey text reads as background against the saturated cyan/orange sparkline lines.

## Plan

- [x] Swap header positions: battery + voltage to top-left; version/path slot to top-right
- [x] Extend CPU sparkline rectangle to full header height (y=0–41, 42 px tall); bar mapping uses the new height
- [x] Render order: sparkline drawn after text so it overlays battery/path/version where they cross
- [x] Bump `APP_VERSION` from `0.16.2` to `0.16.3`
- [x] On-device verification: battery+voltage on left, path/version on right, both clearly readable when CPU is low; sparkline visibly overlays the right portion of row 1 under high CPU; `` ` `` toggle still works; no UI regressions

## Log

- 2026-05-10: Implementation needed a render-order refactor not anticipated in the Plan. `drawCpuOverlay` previously did its own background fill to wipe last frame's sparkline; with the graph now extending into row 1, that fill would erase the path text underneath. Removed the fill; `drawHeader` is now the sole owner of the header rectangle's clear-and-redraw cycle. `pollDiagnostics` calls `drawHeader` (whole header refresh) instead of `drawDiagnosticsRow` so path text stays fresh under the sparkline.
- 2026-05-10: User-test refinements landed without intermediate patch bumps (process slip — refinements should each bump patch). Path text recoloured from mid-grey to white for legibility under sparkline overlap; path left-justifies after voltage with a 10 px gap (4 px small + 1 char breathing room); version stays right-justified at the screen edge until first keypress, after which the path takes the slot left-justified. Final shipped version stays at `0.16.3`.

## Conclusion

Shipped. Header swap and graph-extension landed; the draw-over-path approach reads as predicted (sparkline lines climb into the path region only under high CPU, leaving it clean at normal load). One render-order refactor was needed during build to make `drawHeader` the sole owner of the header clear; not in the Plan but the policy doesn't hold without it.

Final shipped version: `0.16.3`.

### Proposed `CHANGELOG.md` entry

```
## 0.16.3 — 2026-05-10

- Header rearranged: battery + voltage on the top-left, version/path on the top-right. With diagnostics showing, the CPU sparkline now uses the full header height — lines overlay the path region under high CPU, leaving it clean at normal load.
```

### Map nodes touched (post-archive per-node negotiation)

Five nodes need updating: **Header** (row 1 layout swap), **Battery** (now top-left), **Version** (right-justified at the edge), **Path Breadcrumb** (left-justified after voltage), **Diagnostics** (sparkline rectangle now full header height + overlay behaviour).
