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
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/dual_os}"
MEDIA_MINIMAL="${TDL_APP_MEDIA_MINIMAL:-OFF}"

if [ ! -d "${THIRD_PARTY_DIR}" ]; then
  echo "Missing third-party bundle: ${THIRD_PARTY_DIR}" >&2
  echo "Set TDL_APP_THIRD_PARTY_DIR or place cv184x under ${PROJECT_ROOT}/third_party" >&2
  exit 1
fi

echo "TDL_APP_THIRD_PARTY_DIR=${THIRD_PARTY_DIR}"
echo "TDL_APP_MEDIA_MINIMAL=${MEDIA_MINIMAL}"

cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/toolchains/arm-none-linux-musleabihf.cmake" \
  -DTOOLCHAIN_ROOT="${TOOLCHAIN_ROOT}" \
  -DTDL_APP_THIRD_PARTY_DIR="${THIRD_PARTY_DIR}" \
  -DTDL_APP_MEDIA_MINIMAL="${MEDIA_MINIMAL}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"
cmake --install "${BUILD_DIR}"

printf 'Install dir: %s\n' "${INSTALL_DIR}"

