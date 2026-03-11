#!/usr/bin/env bash
# Build and flash the S3 firmware (ESP32-S3, Wi-Fi SlimProto player).
# Usage: ./flash-s3.sh [PORT]
#   PORT defaults to /dev/ttyUSB0
set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

# Source ESP-IDF environment
if [[ -z "${IDF_PATH:-}" ]]; then
  if [[ -f "$ROOT_DIR/esp-idf-v5.5.3/export.sh" ]]; then
    . "$ROOT_DIR/esp-idf-v5.5.3/export.sh"
  else
    echo "ERROR: IDF_PATH not set and esp-idf-v5.5.3/export.sh not found." >&2
    exit 1
  fi
fi

echo "==> Building S3 firmware..."
idf.py -C S3-BT build

echo "==> Flashing to $PORT and starting monitor (Ctrl-] to exit)..."
idf.py -C S3-BT -p "$PORT" flash monitor
