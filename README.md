# cardplayer

Audio media player for the [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv). Plays WAV/MP3/FLAC/AAC from a FAT-formatted microSD card.

Built with PlatformIO + Arduino framework + [M5Unified](https://github.com/m5stack/M5Unified).

## First-time setup

See [setup.md](setup.md) for installing PlatformIO and configuring USB permissions on Ubuntu.

## Day-to-day commands

Run from the repo root.

```bash
pio run                    # build
pio run -t upload          # build + flash the connected Cardputer
pio device monitor         # open serial monitor (Ctrl-C to exit)
pio run -t clean           # wipe build artifacts

pio run -t upload -t monitor
```

If `upload` can't auto-detect the board, specify the port:

```bash
pio run -t upload --upload-port /dev/ttyACM0
```

List serial devices PlatformIO can see:

```bash
pio device list
```

## Target

- Board: `m5stack-stamps3` (the Stamp-S3A module inside the Cardputer ADV)
- MCU: ESP32-S3, 8 MB flash
- Audio: ES8311 I2S codec → 1 W speaker / 3.5 mm jack
