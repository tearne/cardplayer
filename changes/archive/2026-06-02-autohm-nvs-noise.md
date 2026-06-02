# Remove obsolete autohm settings migration

**Mode:** Wander

## Intent

The console logged `nvs_erase_key fail: autohm NOT_FOUND` on every settings flush: the one-shot migration that removed the legacy `autohm` key (renamed to `autosp`) ran unconditionally in `flushStateIfDirty`, so once the key was gone it errored on every flush.

The migration is no longer needed at all — both current installs post-date the rename, so the legacy key isn't present on either device. Rather than guard the removal, delete the whole migration: the load-time fallback to `autohm` and the `remove("autohm")` cleanup both go, leaving `autosp` read directly.

## Conclusion

Completed at 0.24.5. Removed the load-time `autohm` fallback and the unconditional `remove("autohm")`; `g_auto_spectrum` now reads `autosp` directly. No changelog entry — internal log-noise / dead-migration cleanup, no user-facing change.
