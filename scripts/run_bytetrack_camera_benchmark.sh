#!/bin/sh
set -eu

DIR=$(cd "$(dirname "$0")/.." && pwd)
export TDL_APP_PROJECT_ROOT="${DIR}"
. "${DIR}/env.sh"
RESULT_DIR=${RESULT_DIR:-/tmp/jyd_results}
mkdir -p "${RESULT_DIR}"

exec "${DIR}/bin/tdl_byte_tracker_camera_demo" \
  --model-spec "${DIR}/configs/model_specs/yolov8n_det_coco80.mud" \
  --class-id 0 --line-x 320 \
  --high-score 0.45 --low-score 0.15 --iou-threshold 0.30 \
  --group 0 --channel 1 --warmup 30 --frames 300 \
  --dump-frame "${RESULT_DIR}/bytetrack_camera_input.jpg" \
  --dump-overlay "${RESULT_DIR}/bytetrack_camera_overlay.jpg" \
  "$@"
