# SD-card firmware installer

**Mode:** Formal

## Intent

Let users update firmware by copying a `.bin` to the SD card and installing it from within the app, rather than needing PlatformIO and a USB toolchain. The SD card is the *transport* for the firmware image — the chip still boots from internal flash. Storing multiple `.bin` files on the card lets the user pick which to install (e.g., re-installing a known-good version to roll back from a bad release).

## Approach

### UI-initiated install from a dedicated view

A new view inside the app lists every `.bin` file under a known folder (`/FW/` on the SD card). Selecting one shows a confirm dialog with the file size and a brief warning; confirming triggers the flash-and-reboot sequence. No automatic boot-time scanning of the SD card — the install is always a deliberate user action.

### Two-OTA-partition flow

The 8 MB flash layout (`default_8MB.csv`) has two OTA partitions plus the bootloader. The app reads the chosen `.bin` from SD in chunks, writes it to the *inactive* OTA partition via `esp_ota_*`, sets that partition as the next boot target, and reboots.

The previously-active firmware remains in the other partition. A "boot the other partition" affordance gives instant rollback without re-reading from SD — provided the other partition still holds a working image.

### Two new affordances

- An entry into the install view. Cheapest: a dedicated key combo (e.g. `Fn+u`). Better long-term: an entry in a future settings screen. The change builds the view; the wiring through a settings screen can wait.
- A "boot other partition" action — same path. A second key combo (e.g. `Fn+r`) or a separate settings entry.

### Validation safety net — out of scope for this change

ESP-IDF supports "rollback if first boot doesn't confirm" semantics (`esp_ota_mark_app_invalid_rollback_and_reboot`). Useful but adds state (the app must learn "I'm fresh, prove I work"). Defer; the two-OTA-partition rollback covers most user need.

### Failure handling

If the SD read fails partway through, abort and don't switch the boot target. The user lands back in the current firmware on next boot. If the flash write fails, same thing — `esp_ota_end` only commits if the write was clean.

### Memory and timing

- ~1 MB of flash write at ~200 KB/s ≈ 5 s. Display a progress bar.
- Read buffer 4 KB on stack (or static). Negligible RAM cost.
- During flash, audio task is paused / playback torn down (the flash write blocks the SPI bus and is incompatible with concurrent audio).

## Unresolved

- **Entry-point**: dedicated key combo (`Fn+u` and `Fn+r`) now, or wait for a settings screen that doesn't exist yet?
- **Where does the user *put* `.bin` files**: hard-coded `/FW/` directory, or any `.bin` in the library tree?
- **Filename conventions** for showing version info to the user — strict (`firmware-0.17.39.bin`), free-form, or just sort by mtime?
- **Validation safety net** — accept the deferral, or fold it into this change?
