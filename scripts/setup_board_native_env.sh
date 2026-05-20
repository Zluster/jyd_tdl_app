#!/bin/sh
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-${PROJECT_ROOT}/third_party/cv184x}"

mkdir -p "${THIRD_PARTY_DIR}/firmware"

# Make source-tree model / firmware paths match the packaged runtime layout.
ln -sfn "third_party/cv184x/models" "${PROJECT_ROOT}/models"
ln -sfn "third_party/cv184x/firmware" "${PROJECT_ROOT}/firmware"

# libtdl_core.so keeps NEEDED entries on libopencv_*.so.4.5.
for name in core imgproc imgcodecs; do
  compat="/usr/lib/libopencv_${name}.so.4.5"
  if [ -e "${compat}" ]; then
    continue
  fi
  target=$(find /usr/lib -maxdepth 1 -name "libopencv_${name}.so.4.5.*" | sort | tail -n 1)
  if [ -n "${target}" ]; then
    ln -sfn "${target}" "${compat}"
    echo "created ${compat} -> ${target}"
  else
    echo "missing system OpenCV target for ${name}" >&2
  fi
done

echo "board native environment is ready"
echo "project root: ${PROJECT_ROOT}"
echo "third-party dir: ${THIRD_PARTY_DIR}"
