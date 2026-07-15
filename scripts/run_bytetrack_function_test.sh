#!/bin/sh
set -eu

DIR=$(cd "$(dirname "$0")/.." && pwd)
export TDL_APP_PROJECT_ROOT="${DIR}"
. "${DIR}/env.sh"
RESULT_DIR=${RESULT_DIR:-/tmp/jyd_results}
mkdir -p "${RESULT_DIR}"

# The original frames are 2732x1534.  This functional test uses their
# pre-generated 640x360 copies so JPEG decoding does not dominate ByteTrack.
exec "${DIR}/bin/tdl_byte_tracker_sequence_demo" \
  --frames-dir "${DIR}/assets/tracker_synthetic/frames_640" \
  --model-spec "${DIR}/configs/model_specs/yolov8n_det_coco80.mud" \
  --start 0 --frames 100 --filename-width 4 --extension .jpg \
  --class-id 0 --line-x 240 \
  --output "${RESULT_DIR}/bytetrack_function_overlay.jpg" \
  "$@"
