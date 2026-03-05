# Bluetooth Music Player (CYD + S3 Variants)

This repository contains two separate firmware targets:

- CYD build (ESP32 Classic Bluetooth A2DP sink) using [CYD-BT](CYD-BT)
- S3 build (ESP32-S3 Wi-Fi SlimProto player) using [S3-BT](S3-BT)

Both targets share the same repository, but they are intended for different hardware and playback modes.

## Common Setup

1. Install and export ESP-IDF (v5.5.x recommended).
2. Open this repo root.
3. Use the variant-specific build commands below.

## Web Flasher (GitHub Pages)

The repository includes a browser flasher app in [web-flasher](web-flasher).

- UI supports both firmware options: CYD and S3.
- One-click flashing uses `esp-web-tools` with these manifests:
  - [web-flasher/manifest-cyd.json](web-flasher/manifest-cyd.json)
  - [web-flasher/manifest-s3.json](web-flasher/manifest-s3.json)
- CYD prebuilt install options include default, touch disabled, image loading disabled, and touch + image disabled.
- Build config section generates a shell script for local custom builds.
- Custom flash mode supports offset/path input and generated manifests.
- CYD config includes build-size toggles:
  - Disable touch module (`CYD_ENABLE_TOUCH=OFF`)
  - Disable image loading / cover-art decode (`CYD_ENABLE_IMAGE_LOADING=OFF`)

Hosted URL pattern after enabling GitHub Pages:

- `https://<your-org-or-user>.github.io/<repo-name>/`

Notes:

- Browser flashing uses prebuilt binaries placed under `web-flasher/firmware/...`.
- Custom values (device name / S3 network settings) are applied via the generated script, then firmware is built and flashed locally.
- Workflow [deploy-web-flasher.yml](.github/workflows/deploy-web-flasher.yml) publishes Pages content from `web-flasher`.
- Workflow [release-web-flasher.yml](.github/workflows/release-web-flasher.yml) can auto-build CYD/S3 binaries and publish a ready-to-flash Pages site.

---

## CYD Section (ESP32)

### What it is

CYD is the Classic Bluetooth music player build for ESP32-based Cheap Yellow Display boards.

- Target: `esp32`
- Playback: Bluetooth A2DP sink from phones/tablets
- Control: AVRCP (play/pause/next/prev + metadata)
- Display stack: ST7789 + touch UI in [CYD-BT](CYD-BT)

### CYD Hardware Profile

- Board: ESP32 CYD 2.8" class board
- LCD: ST7789 (SPI)
- Touch: FT6336 (I2C)
- Audio: I2S output path used by current firmware

### CYD Pin Notes (current profile)

Display (SPI):

- MISO: GPIO 12
- MOSI: GPIO 13
- CLK: GPIO 14
- CS: GPIO 15
- DC: GPIO 2
- Backlight: GPIO 21

Touch (I2C):

- SDA: GPIO 33
- SCL: GPIO 32
- INT: GPIO 36
- RST: not connected

Audio (I2S):

- BCK: GPIO 26
- WS: GPIO 27
- DOUT: GPIO 25

### Build / Flash (CYD)

```bash
cd /path/to/cyd-espidf
. ./esp-idf-v5.5.3/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Helper scripts (repo root):

- `./flash.sh`
- `./monitor.sh`

### Use (CYD)

1. Boot device.
2. Pair phone to `CYD Music Player`.
3. Start playback on phone.
4. Use on-screen touch controls for transport.

---

## S3 Section (ESP32-S3)

### What it is

S3 is a separate firmware target for the ESP32-S3 ES3C28P-style board profile.

- Target: `esp32s3`
- Playback: Wi-Fi SlimProto player for Music Assistant
- Optional input path: custom BLE PCM ingest
- Project path: [S3-BT](S3-BT)

Important limitation:

- ESP32-S3 does not support Classic Bluetooth A2DP sink in ESP-IDF, so this target is not a phone A2DP sink like CYD.

### S3 Hardware Profile

- Board profile: LCDWiki ES3C28P
- LCD: ILI9341V
- Touch: FT6336G
- Audio codec: ES8311

### Build / Flash (S3)

```bash
cd /path/to/cyd-espidf
. ./esp-idf-v5.5.3/export.sh
idf.py -C S3-BT set-target esp32s3
idf.py -C S3-BT build
idf.py -C S3-BT -p /dev/ttyUSB0 flash monitor
```

### Use (S3)

1. Configure Wi-Fi and SlimProto settings in [S3-BT/main/main.h](S3-BT/main/main.h):
   - `MA_WIFI_SSID`
   - `MA_WIFI_PASSWORD`
   - `MA_SLIMPROTO_HOST`
   - `MA_SLIMPROTO_PORT`
2. Flash and boot S3 firmware.
3. Add/enable SlimProto player integration in Music Assistant.
4. Start playback to the S3 player.

### Optional BLE Audio Ingest (S3)

- Service UUID: `0x00FF`
- Characteristics:
  - Config: `0xFF01`
  - Audio data: `0xFF02`
- Config payload:
  - Raw: `[channels][sample_rate_le_lo][sample_rate_le_hi]`
  - Framed: `[0x01][channels][sample_rate_le_lo][sample_rate_le_hi]`
- Audio payload:
  - Raw PCM16LE bytes
  - Framed: `[0x02][pcm16le_bytes...]`

---

## Repository Layout

```text
.
├── CYD-BT/          # Active CYD runtime component
├── S3-BT/           # Separate ESP32-S3 project tree
├── CMakeLists.txt   # Root project for CYD build
├── sdkconfig*       # Root config files for CYD build
└── README.md
```

## Troubleshooting

- Serial permission: `sudo usermod -a -G dialout $USER` and re-login.
- Flash connect failures: hold BOOT, tap RESET, release BOOT when flashing starts.
- Check active port: `ls /dev/ttyUSB* /dev/ttyACM*`.
- CYD target mismatch: ensure `idf.py set-target esp32` at repo root.
- S3 target mismatch: ensure `idf.py -C S3-BT set-target esp32s3`.