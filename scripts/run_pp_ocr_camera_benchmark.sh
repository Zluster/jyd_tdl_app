#!/bin/sh
set -eu

DIR=$(cd "$(dirname "$0")/.." && pwd)
export TDL_APP_PROJECT_ROOT="${DIR}"
. "${DIR}/env.sh"
RESULT_DIR=${RESULT_DIR:-/tmp/jyd_results}
mkdir -p "${RESULT_DIR}"

exec "${DIR}/bin/tdl_pp_ocr_camera_demo" \
  --model-spec "${DIR}/configs/model_specs/pp_ocr.mud" \
  --font "${DIR}/fonts/DroidSansFallbackFull.ttf" \
  --group 0 --channel 1 --warmup 30 --frames 300 \
  --dump-frame "${RESULT_DIR}/ocr_camera_input.jpg" \
  --dump-overlay "${RESULT_DIR}/ocr_camera_overlay.jpg" \
  "$@"
