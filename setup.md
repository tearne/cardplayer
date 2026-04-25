# Setup — Ubuntu 24.04 (CLI only)

One-time setup to build and flash code to the **M5Stack Cardputer ADV** from the terminal. Entirely CLI — no IDE.

Rough order:

1. Install PlatformIO Core (`pio`)
2. Give your user permission to talk to USB serial devices
3. Plug in the Cardputer and confirm it's detected

---

## 1. Install PlatformIO Core

Ubuntu 24.04 ships Python 3.12 with PEP 668 enabled, so you can't `pip install` system-wide. Use **pipx**, which installs Python CLI tools in their own isolated environments. It's the clean, boring way.

```bash
sudo apt update
sudo apt install -y pipx python3-venv
pipx ensurepath
```

`pipx ensurepath` adds `~/.local/bin` to your `PATH`. **Open a new terminal** (or `source ~/.bashrc`) before the next command so `pipx`'s install location is on your PATH.

Install PlatformIO:

```bash
pipx install platformio
```

Verify:

```bash
pio --version
```

You should see something like `PlatformIO Core, version 6.x.x`.

> **Tip — shell completion (optional).** If you want tab-completion for `pio`:
> ```bash
> pio system completion install
> ```

## 2. USB serial permissions

Two things need to be right before you can flash:

### 2a. Add yourself to the `dialout` group

```bash
sudo usermod -aG dialout $USER
```

**You must log out and log back in** (or reboot) for this to take effect. Confirm with:

```bash
groups
```

`dialout` should appear in the list.

### 2b. Install PlatformIO's udev rules

These tell the kernel to grant access to common dev boards, including ESP32-S3.

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## 3. Plug in the Cardputer and confirm it's detected

Connect the Cardputer ADV with a **USB-C data cable** (some cables are power-only — if nothing is detected, try a different cable before debugging anything else). Power the device on.

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

You should see something like `/dev/ttyACM0`. That's the board.

For extra confirmation:

```bash
dmesg | tail -20
```

Look for a recent line mentioning `cdc_acm` and a new `ttyACM` device.

You can also ask PlatformIO to list serial devices it can see:

```bash
pio device list
```

It should print an entry for `/dev/ttyACM0` with a description mentioning Espressif / ESP32-S3.

If **nothing appears**:
- Try a different USB-C cable (most common culprit).
- Try a different USB port (prefer a direct port, not a hub).
- Make sure you logged out/in after the `usermod` step.
- Confirm the Cardputer is powered on (screen lit).

---

## Done

When `pio --version` works and `pio device list` shows the Cardputer, you're ready for **Step 1: Hello world**. Ping me and I'll scaffold the project.
