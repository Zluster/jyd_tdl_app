#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

echo "Cleaning generated directories under: ${PROJECT_ROOT}"
rm -rf \
  "${PROJECT_ROOT}/build" \
  "${PROJECT_ROOT}/install" \
  "${PROJECT_ROOT}/package"

rm -f \
  "${PROJECT_ROOT}/nn_base.hpp" \
  "${PROJECT_ROOT}/nn_yolov5.cpp"

echo "Clean complete."
