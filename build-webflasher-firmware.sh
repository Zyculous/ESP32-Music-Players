#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

if [[ -z "${IDF_PATH:-}" ]]; then
  if [[ -f "$ROOT_DIR/esp-idf-v5.5.3/export.sh" ]]; then
    . "$ROOT_DIR/esp-idf-v5.5.3/export.sh"
  else
    echo "IDF_PATH is not set and local export script was not found at esp-idf-v5.5.3/export.sh" >&2
    exit 1
  fi
else
  . "$IDF_PATH/export.sh"
fi

mkdir -p web-flasher/firmware/cyd web-flasher/firmware/s3

CYD_BASE_SDKCONFIG_DEFAULTS="sdkconfig.cyd-base.defaults"
if [[ -f sdkconfig ]]; then
  cp sdkconfig "$CYD_BASE_SDKCONFIG_DEFAULTS"
  CYD_SDKCONFIG_DEFAULTS="$CYD_BASE_SDKCONFIG_DEFAULTS;sdkconfig.cyd-ci.defaults"
elif [[ -f sdkconfig.defaults ]]; then
  CYD_SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.cyd-ci.defaults"
else
  CYD_SDKCONFIG_DEFAULTS="sdkconfig.cyd-ci.defaults"
fi

S3_BASE_SDKCONFIG_DEFAULTS="S3-BT/sdkconfig.s3-base.defaults"
if [[ -f S3-BT/sdkconfig ]]; then
  cp S3-BT/sdkconfig "$S3_BASE_SDKCONFIG_DEFAULTS"
  S3_SDKCONFIG_DEFAULTS="sdkconfig.s3-base.defaults;sdkconfig.s3-ci.defaults"
elif [[ -f S3-BT/sdkconfig.defaults ]]; then
  S3_SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.s3-ci.defaults"
else
  S3_SDKCONFIG_DEFAULTS="sdkconfig.s3-ci.defaults"
fi

cleanup() {
  rm -f "$CYD_BASE_SDKCONFIG_DEFAULTS" "$S3_BASE_SDKCONFIG_DEFAULTS"
}
trap cleanup EXIT

# CYD default
idf.py -B build/cyd-default -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-default -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" build
cp build/cyd-default/bootloader/bootloader.bin web-flasher/firmware/cyd/bootloader.bin
cp build/cyd-default/partition_table/partition-table.bin web-flasher/firmware/cyd/partition-table.bin
cp build/cyd-default/bluetooth_music_player_cyd.bin web-flasher/firmware/cyd/bluetooth_music_player_cyd.bin

# CYD touch disabled
idf.py -B build/cyd-touch-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-touch-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" -D CYD_ENABLE_TOUCH=OFF -D CYD_ENABLE_IMAGE_LOADING=ON build
cp build/cyd-touch-off/bluetooth_music_player_cyd.bin web-flasher/firmware/cyd/bluetooth_music_player_cyd_touch_off.bin

# CYD image loading disabled
idf.py -B build/cyd-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" -D CYD_ENABLE_TOUCH=ON -D CYD_ENABLE_IMAGE_LOADING=OFF build
cp build/cyd-image-off/bluetooth_music_player_cyd.bin web-flasher/firmware/cyd/bluetooth_music_player_cyd_image_off.bin

# CYD touch + image loading disabled
idf.py -B build/cyd-touch-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" set-target esp32
idf.py -B build/cyd-touch-image-off -D SDKCONFIG_DEFAULTS="$CYD_SDKCONFIG_DEFAULTS" -D CYD_ENABLE_TOUCH=OFF -D CYD_ENABLE_IMAGE_LOADING=OFF build
cp build/cyd-touch-image-off/bluetooth_music_player_cyd.bin web-flasher/firmware/cyd/bluetooth_music_player_cyd_touch_image_off.bin

# S3 default
unset IDF_TARGET
idf.py -C S3-BT -D SDKCONFIG_DEFAULTS="$S3_SDKCONFIG_DEFAULTS" set-target esp32s3
idf.py -C S3-BT -D SDKCONFIG_DEFAULTS="$S3_SDKCONFIG_DEFAULTS" build
cp S3-BT/build/bootloader/bootloader.bin web-flasher/firmware/s3/bootloader.bin
cp S3-BT/build/partition_table/partition-table.bin web-flasher/firmware/s3/partition-table.bin
cp S3-BT/build/bluetooth_music_player_es3c28p.bin web-flasher/firmware/s3/bluetooth_music_player_s3.bin

echo "Done. Firmware artifacts updated under web-flasher/firmware/."
