#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
SOPHPI_ROOT="${SOPHPI_ROOT:-/home/jyd/zwz/sophpi}"
OUT_DIR="${OUT_DIR:-${PROJECT_ROOT}/third_party/cv184x}"
TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-${SOPHPI_ROOT}/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf}"
TDL_INSTALL="${SOPHPI_ROOT}/tdl_sdk/install/CV184X"
LIBSOPHON_LIB="${SOPHPI_ROOT}/libsophon/install/libsophon-0.4.9/lib"
CVI_MPI_ROOT="${SOPHPI_ROOT}/cvi_mpi"
CVI_RTSP_ROOT="${SOPHPI_ROOT}/cvi_rtsp"

copy_tree_contents() {
  src="$1"
  dst="$2"
  if [ -d "$src" ]; then
    mkdir -p "$dst"
    cp -a "${src}/." "$dst/"
  else
    echo "WARN: missing directory ${src}" >&2
  fi
}

copy_file() {
  src="$1"
  dst="$2"
  if [ -f "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
  else
    echo "WARN: missing file ${src}" >&2
  fi
}

copy_glob_to_dir() {
  dst="$1"
  shift
  mkdir -p "$dst"
  matched=0
  for pattern in "$@"; do
    for src in $pattern; do
      [ -e "$src" ] || continue
      cp -a "$src" "$dst/"
      matched=1
    done
  done
  if [ "$matched" -eq 0 ]; then
    echo "WARN: no files matched: $*" >&2
  fi
}

rm -rf "${OUT_DIR}"
mkdir -p \
  "${OUT_DIR}/include" \
  "${OUT_DIR}/lib" \
  "${OUT_DIR}/opencv" \
  "${OUT_DIR}/cvi_mpi" \
  "${OUT_DIR}/cvi_rtsp" \
  "${OUT_DIR}/configs" \
  "${OUT_DIR}/models/cv184x" \
  "${OUT_DIR}/firmware" \
  "${OUT_DIR}/runtime"

copy_tree_contents "${TDL_INSTALL}/include" "${OUT_DIR}/include"
copy_tree_contents "${TDL_INSTALL}/sample/tpu/include" "${OUT_DIR}/include"
copy_tree_contents "${SOPHPI_ROOT}/cvi_mpi/include" "${OUT_DIR}/cvi_mpi/include"
copy_tree_contents "${CVI_RTSP_ROOT}/install/include" "${OUT_DIR}/cvi_rtsp/include"
copy_tree_contents "${TDL_INSTALL}/configs/model" "${OUT_DIR}/configs/model"
copy_tree_contents "${TDL_INSTALL}/sample/3rd/opencv/include" "${OUT_DIR}/opencv/include"

for json_src in \
  "${SOPHPI_ROOT}/tdl_sdk/build/CV184X/nlohmannjson-src/json.hpp" \
  "${SOPHPI_ROOT}/tdl_sdk/build/CV184X/_deps/nlohmannjson-src/json.hpp"; do
  if [ -f "$json_src" ]; then
    copy_file "$json_src" "${OUT_DIR}/include/json.hpp"
    break
  fi
done

copy_glob_to_dir "${OUT_DIR}/lib" \
  "${TDL_INSTALL}/lib/libtdl_core.so" \
  "${TDL_INSTALL}/sample/tpu/lib/libmodel_combine.so" \
  "${TDL_INSTALL}/sample/cvi_mpi/lib/"'*.so*' \
  "${CVI_MPI_ROOT}/lib/"'*.so*' \
  "${CVI_RTSP_ROOT}/install/lib/"'*.so*' \
  "${LIBSOPHON_LIB}/libbmrt.so"* \
  "${LIBSOPHON_LIB}/libbmlib.so"* \
  "${TDL_INSTALL}/sample/3rd/zlib/lib/libz.so"* \
  "${TOOLCHAIN_ROOT}/arm-none-linux-musleabihf/lib/libstdc++.so"* \
  "${TOOLCHAIN_ROOT}/arm-none-linux-musleabihf/lib/libgcc_s.so"* \
  "${TOOLCHAIN_ROOT}/arm-none-linux-musleabihf/sysroot/lib/ld-musl-armhf.so.1" \
  "${TOOLCHAIN_ROOT}/arm-none-linux-musleabihf/sysroot/lib/libc.so"

copy_glob_to_dir "${OUT_DIR}/opencv/lib" \
  "${TDL_INSTALL}/sample/3rd/opencv/lib/libopencv_core.so"* \
  "${TDL_INSTALL}/sample/3rd/opencv/lib/libopencv_imgproc.so"* \
  "${TDL_INSTALL}/sample/3rd/opencv/lib/libopencv_imgcodecs.so"*

copy_glob_to_dir "${OUT_DIR}/models/cv184x" \
  "${SOPHPI_ROOT}/tdl_algo_verify/models/"'*.bmodel' \
  "${SOPHPI_ROOT}/ipcamera/resource/ai_model/"'*.bmodel'

for src in \
  "${SOPHPI_ROOT}/libsophon/tpu-kernel/lib/musl_arm/libtpu_kernel_module.so" \
  "${SOPHPI_ROOT}/libsophon/tpu-kernel/lib/arm-linux/libtpu_kernel_module.so" \
  "${SOPHPI_ROOT}/libsophon/tpu-kernel/lib/libtpu_kernel_module.so"; do
  if [ -f "$src" ]; then
    copy_file "$src" "${OUT_DIR}/firmware/libbm1688_kernel_module.so"
    break
  fi
done

cat > "${OUT_DIR}/runtime/env.sh" <<'EOF'
#!/bin/sh
DIR=$(cd "$(dirname "$0")/.." && pwd)
export LD_LIBRARY_PATH="${DIR}/lib:${DIR}/opencv/lib:${LD_LIBRARY_PATH:-}"
if [ -f "${DIR}/firmware/libbm1688_kernel_module.so" ]; then
  export BMRUNTIME_USING_FIRMWARE="${DIR}/firmware/libbm1688_kernel_module.so"
fi
EOF
chmod +x "${OUT_DIR}/runtime/env.sh"

echo "Collected CV184X dependency bundle:"
echo "  ${OUT_DIR}"
