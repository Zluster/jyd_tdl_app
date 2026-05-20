#!/bin/sh

# Source this script from the repository root on the target board:
#   cd /mnt/git/jyd_tdl_app
#   . ./env.sh

SELF_PATH="${BASH_SOURCE:-$0}"
case "${SELF_PATH}" in
  /*)
    SELF_DIR=$(dirname "${SELF_PATH}")
    ;;
  */*)
    SELF_DIR=$(cd "$(dirname "${SELF_PATH}")" && pwd)
    ;;
  *)
    SELF_DIR=$(pwd)
    ;;
esac

PROJECT_ROOT="${TDL_APP_PROJECT_ROOT:-${SELF_DIR}}"
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-${PROJECT_ROOT}/third_party/cv184x}"
FIRMWARE_PATH="${BMRUNTIME_USING_FIRMWARE:-${THIRD_PARTY_DIR}/firmware/libbm1688_kernel_module.so}"

export TDL_APP_PROJECT_ROOT="${PROJECT_ROOT}"
export TDL_APP_THIRD_PARTY_DIR="${THIRD_PARTY_DIR}"
export LD_LIBRARY_PATH="${THIRD_PARTY_DIR}/lib:${LD_LIBRARY_PATH:-}"

if [ -f "${FIRMWARE_PATH}" ]; then
  export BMRUNTIME_USING_FIRMWARE="${FIRMWARE_PATH}"
fi
