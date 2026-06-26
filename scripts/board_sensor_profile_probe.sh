#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

cd "${PROJECT_ROOT}"

if [ -f "./env.sh" ]; then
  # shellcheck disable=SC1091
  . ./env.sh
fi

OUTPUT="${1:-sensor_profile_probe.jpg}"
BOOT_INI="${CAMERA_BOOT_INI:-/mnt/sd/sophpi_mmf/config/camera_boot.ini}"

echo "[1/3] sensor profile selection"
./bin/tdl_sensor_profile_probe_demo \
  --camera-boot-ini "${BOOT_INI}" \
  --output "${OUTPUT}"

echo "[2/3] live frame check"
./bin/tdl_camera_capture_demo \
  --use-sensor-media \
  --attach-existing \
  --camera-boot-ini "${BOOT_INI}" \
  --group 0 \
  --channel 2 \
  --width 1280 \
  --height 720 \
  --output live_probe.jpg

echo "[3/3] ai frame check"
./bin/tdl_camera_capture_demo \
  --use-sensor-media \
  --attach-existing \
  --camera-boot-ini "${BOOT_INI}" \
  --group 0 \
  --channel 1 \
  --width 640 \
  --height 640 \
  --pixel-format 2 \
  --output ai_probe.jpg

echo "done:"
echo "  ${OUTPUT}"
echo "  live_probe.jpg"
echo "  ai_probe.jpg"
