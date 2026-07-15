#!/bin/sh
set -eu

DIR=$(cd "$(dirname "$0")/.." && pwd)
export TDL_APP_PROJECT_ROOT="${DIR}"
. "${DIR}/env.sh"
RESULT_DIR=${RESULT_DIR:-/tmp/jyd_results}
mkdir -p "${RESULT_DIR}"

exec "${DIR}/bin/tdl_pp_ocr_demo" \
  --image "${DIR}/assets/ocr_test_card.jpg" \
  --model-spec "${DIR}/configs/model_specs/pp_ocr.mud" \
  --font "${DIR}/fonts/DroidSansFallbackFull.ttf" \
  --output "${RESULT_DIR}/ocr_function_overlay.jpg" \
  "$@"
