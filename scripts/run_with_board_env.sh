#!/bin/sh
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-${PROJECT_ROOT}/third_party/cv184x}"
FIRMWARE_PATH="${BMRUNTIME_USING_FIRMWARE:-${THIRD_PARTY_DIR}/firmware/libbm1688_kernel_module.so}"

if [ "$#" -eq 0 ]; then
  echo "Usage: $0 <command> [args...]" >&2
  exit 1
fi

export TDL_APP_PROJECT_ROOT="${PROJECT_ROOT}"
export TDL_APP_THIRD_PARTY_DIR="${THIRD_PARTY_DIR}"
export LD_LIBRARY_PATH="${THIRD_PARTY_DIR}/lib:${LD_LIBRARY_PATH:-}"

if [ -f "${FIRMWARE_PATH}" ]; then
  export BMRUNTIME_USING_FIRMWARE="${FIRMWARE_PATH}"
fi

exec "$@"
