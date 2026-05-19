#!/bin/sh
DIR=$(cd "$(dirname "$0")/.." && pwd)
export LD_LIBRARY_PATH="${DIR}/lib:${DIR}/opencv/lib:${LD_LIBRARY_PATH:-}"
if [ -f "${DIR}/firmware/libbm1688_kernel_module.so" ]; then
  export BMRUNTIME_USING_FIRMWARE="${DIR}/firmware/libbm1688_kernel_module.so"
fi
