# Web Flasher

This folder contains a static web flasher UI for GitHub Pages.

## What it does

- Provides two firmware targets:
  - CYD (ESP32)
  - S3 (ESP32-S3)
- Uses `esp-web-tools` for browser-based serial flashing.
- Includes a build-config section that generates a script for local custom builds.
- Includes a custom-flash section with three binary inputs (bootloader, partition table, app), each supporting URL or local file upload.
- For CYD, the build-config device name is injected into the app binary seed marker so the Bluetooth name is stored on first boot.
- Prebuilt CYD install options include:
  - Default
  - Touch Disabled
  - Image Loading Disabled
  - Touch + Image Disabled
- CYD script options support compile-time module trimming:
  - Disable touch module (`CYD_ENABLE_TOUCH=OFF`)
  - Disable image loading / JPEG cover decode (`CYD_ENABLE_IMAGE_LOADING=OFF`)

## Required firmware files

Populate these files before publishing:

- `web-flasher/firmware/cyd/bootloader.bin`
- `web-flasher/firmware/cyd/partition-table.bin`
- `web-flasher/firmware/cyd/bluetooth_music_player_cyd.bin`
- `web-flasher/firmware/cyd/bluetooth_music_player_cyd_touch_off.bin`
- `web-flasher/firmware/cyd/bluetooth_music_player_cyd_image_off.bin`
- `web-flasher/firmware/cyd/bluetooth_music_player_cyd_touch_image_off.bin`
- `web-flasher/firmware/s3/bootloader.bin`
- `web-flasher/firmware/s3/partition-table.bin`
- `web-flasher/firmware/s3/bluetooth_music_player_s3.bin`

## Build artifact source examples

CYD build artifacts (repo root build):

- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/bluetooth_music_player_cyd.bin`

S3 build artifacts (S3-BT build):

- `S3-BT/build/bootloader/bootloader.bin`
- `S3-BT/build/partition_table/partition-table.bin`
- `S3-BT/build/bluetooth_music_player_cyd.bin` (rename to `bluetooth_music_player_s3.bin`)

## Hosting

With GitHub Pages workflow enabled, this folder is published directly.
The page entrypoint is `web-flasher/index.html`.

- Workflow [deploy-web-flasher.yml](../.github/workflows/deploy-web-flasher.yml) handles standard Pages deployment.
- Workflow [release-web-flasher.yml](../.github/workflows/release-web-flasher.yml) can build CYD/S3 firmware and publish a Pages artifact with populated `firmware/` binaries.

## Automated release publishing

On GitHub release publish, [release-web-flasher.yml](../.github/workflows/release-web-flasher.yml) builds CYD + S3 firmware and deploys a ready-to-flash Pages artifact.

## UI flow

1. Select firmware target (CYD or S3).
2. Flash prebuilt firmware.
3. (Optional) Generate local build script with custom values.
4. (Optional) Flash custom build by providing bootloader/partition/app binaries via URL or file upload.
