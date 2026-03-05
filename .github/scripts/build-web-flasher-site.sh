#!/usr/bin/env bash
set -euo pipefail

. "$IDF_PATH/export.sh"

mkdir -p site
cp -R web-flasher/* site/

CYD_SDKCONFIG_DEFAULTS="sdkconfig;sdkconfig.cyd-ci.defaults"

# Build CYD default (repo root)
idf.py -B build/cyd-default -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-default -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" build
mkdir -p site/firmware/cyd
cp build/cyd-default/bootloader/bootloader.bin site/firmware/cyd/bootloader.bin
cp build/cyd-default/partition_table/partition-table.bin site/firmware/cyd/partition-table.bin
cp build/cyd-default/bluetooth_music_player_cyd.bin site/firmware/cyd/bluetooth_music_player_cyd.bin

# Build CYD with touch disabled
idf.py -B build/cyd-touch-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-touch-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" -D CMAKE_C_FLAGS="-DCYD_FEATURE_TOUCH=0 -DCYD_FEATURE_IMAGE_LOADING=1" build
cp build/cyd-touch-off/bluetooth_music_player_cyd.bin site/firmware/cyd/bluetooth_music_player_cyd_touch_off.bin

# Build CYD with image loading disabled
idf.py -B build/cyd-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" -D CMAKE_C_FLAGS="-DCYD_FEATURE_TOUCH=1 -DCYD_FEATURE_IMAGE_LOADING=0" build
cp build/cyd-image-off/bluetooth_music_player_cyd.bin site/firmware/cyd/bluetooth_music_player_cyd_image_off.bin

# Build CYD with both touch and image loading disabled
idf.py -B build/cyd-touch-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-touch-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" -D CMAKE_C_FLAGS="-DCYD_FEATURE_TOUCH=0 -DCYD_FEATURE_IMAGE_LOADING=0" build
cp build/cyd-touch-image-off/bluetooth_music_player_cyd.bin site/firmware/cyd/bluetooth_music_player_cyd_touch_image_off.bin

# Build S3 variant
idf.py -C S3-BT set-target esp32s3
idf.py -C S3-BT build
mkdir -p site/firmware/s3
cp S3-BT/build/bootloader/bootloader.bin site/firmware/s3/bootloader.bin
cp S3-BT/build/partition_table/partition-table.bin site/firmware/s3/partition-table.bin
cp S3-BT/build/bluetooth_music_player_es3c28p.bin site/firmware/s3/bluetooth_music_player_s3.bin
