#!/bin/sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="${DIR}/lib:${LD_LIBRARY_PATH:-}"
if [ -f "${DIR}/firmware/libbm1688_kernel_module.so" ]; then
  export BMRUNTIME_USING_FIRMWARE="${DIR}/firmware/libbm1688_kernel_module.so"
fi

exec "${DIR}/bin/tdl_face_dense_keypoint_demo" "$@"
