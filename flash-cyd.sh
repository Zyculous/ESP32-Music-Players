#!/usr/bin/env bash
# Build and flash CYD firmware (ESP32, Classic BT A2DP sink).
# Usage:
#   ./flash-cyd.sh [PORT]
#   ./flash-cyd.sh --image [PORT]
# Default mode keeps image loading OFF for non-PSRAM boards.
set -euo pipefail

IMAGE_MODE=0
PORT="/dev/ttyUSB0"

if [[ "${1:-}" == "--image" ]]; then
  IMAGE_MODE=1
  shift
fi

if [[ -n "${1:-}" ]]; then
  PORT="$1"
fi
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

if [[ "$IMAGE_MODE" -eq 1 ]]; then
  echo "==> Building CYD firmware with image loading ON (requires working PSRAM)..."
  idf.py -D CYD_ENABLE_IMAGE_LOADING=ON build
else
  echo "==> Building CYD firmware with image loading OFF (default, PSRAM-safe)..."
  idf.py -D CYD_ENABLE_IMAGE_LOADING=OFF build
fi

echo "==> Flashing to $PORT and starting monitor (Ctrl-] to exit)..."
idf.py -p "$PORT" flash monitor
