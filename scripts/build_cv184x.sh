#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-/home/jyd/zwz/sophpi/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf}"
if [ -z "${TOOLCHAIN_ROOT:-}" ] || [ ! -d "${TOOLCHAIN_ROOT}" ]; then
  if [ -d "${PROJECT_ROOT}/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf" ]; then
    TOOLCHAIN_ROOT="${PROJECT_ROOT}/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf"
  fi
fi
if [ ! -d "${TOOLCHAIN_ROOT}" ]; then
  echo "Missing toolchain root: ${TOOLCHAIN_ROOT}" >&2
  echo "Set TOOLCHAIN_ROOT or place host-tools under ${PROJECT_ROOT}/host-tools" >&2
  exit 1
fi
export TOOLCHAIN_ROOT
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build/cv184x}"
INSTALL_DIR="${INSTALL_DIR:-${PROJECT_ROOT}/install/cv184x}"
TDL_APP_PROFILE="${TDL_APP_PROFILE:-single_os}"
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-}"
if [ -z "${THIRD_PARTY_DIR}" ]; then
  case "${TDL_APP_PROFILE}" in
    single_os)
      if [ -d "/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/single_os" ]; then
        THIRD_PARTY_DIR="/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/single_os"
      elif [ -d "${PROJECT_ROOT}/third_party/cv184x" ]; then
        THIRD_PARTY_DIR="${PROJECT_ROOT}/third_party/cv184x"
      fi
      ;;
    dual_os)
      if [ -d "/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/dual_os" ]; then
        THIRD_PARTY_DIR="/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/dual_os"
      fi
      ;;
    *)
      echo "Unsupported TDL_APP_PROFILE: ${TDL_APP_PROFILE}" >&2
      echo "Use TDL_APP_PROFILE=single_os or TDL_APP_PROFILE=dual_os" >&2
      exit 1
      ;;
  esac
fi

if [ ! -d "${THIRD_PARTY_DIR}" ]; then
  echo "Missing third-party bundle: ${THIRD_PARTY_DIR}" >&2
  echo "Set TDL_APP_THIRD_PARTY_DIR or place cv184x under ${PROJECT_ROOT}/third_party" >&2
  exit 1
fi

echo "TDL_APP_PROFILE=${TDL_APP_PROFILE}"
echo "TDL_APP_THIRD_PARTY_DIR=${THIRD_PARTY_DIR}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/toolchains/arm-none-linux-musleabihf.cmake" \
  -DTOOLCHAIN_ROOT="${TOOLCHAIN_ROOT}" \
  -DTDL_APP_THIRD_PARTY_DIR="${THIRD_PARTY_DIR}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"
cmake --install "${BUILD_DIR}"

echo "Install dir: ${INSTALL_DIR}"
