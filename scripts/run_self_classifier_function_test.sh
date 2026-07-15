#!/bin/sh
set -eu

DIR=$(cd "$(dirname "$0")/.." && pwd)
export TDL_APP_PROJECT_ROOT="${DIR}"
. "${DIR}/env.sh"
RESULT_DIR=${RESULT_DIR:-/tmp/jyd_results}
mkdir -p "${RESULT_DIR}"
BANK="${RESULT_DIR}/self_classifier_test.bank"
rm -f "${BANK}"

exec "${DIR}/bin/tdl_self_learn_classify_demo" \
  --model-spec "${DIR}/configs/model_specs/feature_clip_image.mud" \
  --bank "${BANK}" \
  --add "dog=${DIR}/assets/dog.jpg" \
  --add "plant=${DIR}/assets/plant.jpg" \
  --image "${DIR}/assets/self_learning_dog_query.jpg" \
  --top-k 2 \
  --output "${RESULT_DIR}/self_classifier_function_overlay.jpg" \
  "$@"
