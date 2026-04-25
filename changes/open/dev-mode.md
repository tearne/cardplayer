# Developer mode

## Intent

`BATTERY_DEV_MODE` (introduced in `battery-stuck-blue`) currently only shifts the battery thresholds upward so the empty-state path is easy to exercise. Promote it into a project-wide developer-mode flag that *also* gates visibility of in-app diagnostics — stack utilisation, ring-buffer fill, underrun count, and similar readouts — so the production presentation is clean while the developer-facing detail remains a one-line edit away.
