#!/usr/bin/env bash
set -euo pipefail

if [ -f "./CMakeLists.txt" ] && [ -d "./third_party/cv184x/dual_os" ]; then
  PROJECT_ROOT=$(pwd)
else
  SCRIPT_PATH="${BASH_SOURCE:-$0}"
  SCRIPT_DIR=$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)
  PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
fi
TDL_APP_PROFILE="${TDL_APP_PROFILE:-dual_os}"
if [ "${TDL_APP_PROFILE}" != "dual_os" ]; then
  echo "Unsupported TDL_APP_PROFILE: ${TDL_APP_PROFILE}" >&2
  echo "jyd_tdl_app only supports dual_os packaging" >&2
  exit 1
fi
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-}"
if [ -z "${THIRD_PARTY_DIR}" ]; then
  if [ -d "/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/dual_os" ]; then
    THIRD_PARTY_DIR="/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x/dual_os"
  else
    THIRD_PARTY_DIR="${PROJECT_ROOT}/third_party/cv184x/dual_os"
  fi
fi
DEFAULT_PKG_DIR="${PROJECT_ROOT}/package/tdl_app_sdk_cv184x"
FALLBACK_PKG_DIR="${PROJECT_ROOT}/package_user/tdl_app_sdk_cv184x"

if [ -n "${PKG_DIR:-}" ]; then
  RESOLVED_PKG_DIR="${PKG_DIR}"
elif [ -e "${DEFAULT_PKG_DIR}" ] && [ ! -w "${DEFAULT_PKG_DIR}" ]; then
  RESOLVED_PKG_DIR="${FALLBACK_PKG_DIR}"
elif [ -d "${PROJECT_ROOT}/package" ] && [ ! -w "${PROJECT_ROOT}/package" ]; then
  RESOLVED_PKG_DIR="${FALLBACK_PKG_DIR}"
else
  RESOLVED_PKG_DIR="${DEFAULT_PKG_DIR}"
fi

if [ -n "${INSTALL_DIR:-}" ]; then
  RESOLVED_INSTALL_DIR="${INSTALL_DIR}"
elif [ -d "${PROJECT_ROOT}/install_user/cv184x" ]; then
  RESOLVED_INSTALL_DIR="${PROJECT_ROOT}/install_user/cv184x"
else
  RESOLVED_INSTALL_DIR="${PROJECT_ROOT}/install/cv184x"
fi

if [ ! -d "${RESOLVED_INSTALL_DIR}" ]; then
  echo "Missing install dir: ${RESOLVED_INSTALL_DIR}" >&2
  echo "Set INSTALL_DIR or run scripts/build_cv184x.sh first" >&2
  exit 1
fi

if [ ! -d "${THIRD_PARTY_DIR}" ]; then
  echo "Missing third-party bundle: ${THIRD_PARTY_DIR}" >&2
  echo "Set TDL_APP_THIRD_PARTY_DIR or export TDL_APP_PROFILE correctly" >&2
  exit 1
fi

rm -rf "${RESOLVED_PKG_DIR}"
mkdir -p "${RESOLVED_PKG_DIR}/bin" "${RESOLVED_PKG_DIR}/lib" "${RESOLVED_PKG_DIR}/models" "${RESOLVED_PKG_DIR}/configs" "${RESOLVED_PKG_DIR}/firmware" "${RESOLVED_PKG_DIR}/assets"

cp -a "${RESOLVED_INSTALL_DIR}/bin/." "${RESOLVED_PKG_DIR}/bin/" 2>/dev/null || true
cp -a "${RESOLVED_INSTALL_DIR}/lib/." "${RESOLVED_PKG_DIR}/lib/" 2>/dev/null || true
cp -a "${RESOLVED_INSTALL_DIR}/configs/." "${RESOLVED_PKG_DIR}/configs/" 2>/dev/null || true
cp -a "${THIRD_PARTY_DIR}/lib/." "${RESOLVED_PKG_DIR}/lib/"
# PROJECT_MODELS_DIR="${PROJECT_ROOT}/third_party/cv184x/models"
# if [ -d "${PROJECT_MODELS_DIR}" ]; then
#   cp -a "${PROJECT_MODELS_DIR}/." "${RESOLVED_PKG_DIR}/models/"
# fi
SHERPA_KWS_BMRT="${PROJECT_ROOT}/third_party/vendor/sherpa_onnx/lib/libsherpa-onnx-cv184x-bmrt.so"
if [ -f "${SHERPA_KWS_BMRT}" ]; then
  cp -a "${SHERPA_KWS_BMRT}" "${RESOLVED_PKG_DIR}/lib/"
fi
# 复制 OpenCV 库（如果存在独立的 opencv 目录）
if [ -d "${THIRD_PARTY_DIR}/opencv/lib" ]; then
  cp -a "${THIRD_PARTY_DIR}/opencv/lib/." "${RESOLVED_PKG_DIR}/lib/"
fi
#cp -a "${THIRD_PARTY_DIR}/models/." "${RESOLVED_PKG_DIR}/models/"
# 复制模型文件
# MODELS_DIR="${PROJECT_ROOT}/third_party/cv184x/models"
# if [ -d "${MODELS_DIR}" ]; then
#   mkdir -p "${RESOLVED_PKG_DIR}/third_party/cv184x/models"
#   cp -a "${MODELS_DIR}/." "${RESOLVED_PKG_DIR}/third_party/cv184x/models/"
#   echo "Copied models from ${MODELS_DIR}"
# else
#   echo "Warning: Models directory not found: ${MODELS_DIR}" >&2
# fi
cp -a "${THIRD_PARTY_DIR}/firmware/." "${RESOLVED_PKG_DIR}/firmware/" 2>/dev/null || true
cp -a "${PROJECT_ROOT}/assets/." "${RESOLVED_PKG_DIR}/assets/" 2>/dev/null || true
if [ -f "${PROJECT_ROOT}/tools/kws_keyword_registry.py" ]; then
  mkdir -p "${RESOLVED_PKG_DIR}/tools"
  cp -a "${PROJECT_ROOT}/tools/kws_keyword_registry.py" "${RESOLVED_PKG_DIR}/tools/"
  if [ -d "${PROJECT_ROOT}/third_party/vendor/pypinyin" ]; then
    cp -a "${PROJECT_ROOT}/third_party/vendor/pypinyin" "${RESOLVED_PKG_DIR}/tools/"
  fi
fi
if [ -f "${PROJECT_ROOT}/configs/kws_registry.default.json" ]; then
  cp -a "${PROJECT_ROOT}/configs/kws_registry.default.json" \
    "${RESOLVED_PKG_DIR}/kws_registry.json"
fi
if [ -f "${PROJECT_ROOT}/configs/kws_keywords.default.txt" ]; then
  cp -a "${PROJECT_ROOT}/configs/kws_keywords.default.txt" \
    "${RESOLVED_PKG_DIR}/kws_keywords.txt"
fi

cat > "${RESOLVED_PKG_DIR}/env.sh" <<'EOF'
#!/bin/sh
DIR=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="${DIR}/lib:${LD_LIBRARY_PATH:-}"
if [ -d "${DIR}/lib/python" ]; then
  export PYTHONPATH="${DIR}/lib/python:${PYTHONPATH:-}"
fi
if [ -f "${DIR}/firmware/libbm1688_kernel_module.so" ]; then
  export BMRUNTIME_USING_FIRMWARE="${DIR}/firmware/libbm1688_kernel_module.so"
fi
EOF
chmod +x "${RESOLVED_PKG_DIR}/env.sh"

cat > "${RESOLVED_PKG_DIR}/run_detect_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_detect_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_detect_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_yolov5_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_yolov5_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_yolov5_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_yolov8_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_yolov8_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_yolov8_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_classify_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_classify_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_classify_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_feature_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_feature_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_feature_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_keypoint_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_keypoint_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_keypoint_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_semantic_seg_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_semantic_seg_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_semantic_seg_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_instance_seg_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_instance_seg_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_instance_seg_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_lane_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_lane_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_lane_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_vad_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_vad_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_vad_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_speaker_recognition_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
cd "${DIR}"
exec "${DIR}/bin/tdl_speaker_recognition_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_speaker_recognition_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_npu_asr_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
cd "${DIR}"
exec "${DIR}/bin/tdl_npu_asr_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_npu_asr_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_npu_keyword_spotter_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
cd "${DIR}"
exec "${DIR}/bin/tdl_npu_keyword_spotter_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_npu_keyword_spotter_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_camera_capture_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_camera_capture_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_camera_capture_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_camera_detect_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_camera_detect_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_camera_detect_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_sophpi_ai_osd_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/sophpi_ai_osd_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_sophpi_ai_osd_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_camera_classify_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_camera_classify_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_camera_classify_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_camera_feature_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_camera_feature_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_camera_feature_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_multi_vpss_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_multi_vpss_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_multi_vpss_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_media_smoke_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_media_smoke_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_media_smoke_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_vdec_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_vdec_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_vdec_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_osd_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_osd_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_osd_demo.sh"

cat > "${RESOLVED_PKG_DIR}/run_pp_ocr_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_pp_ocr_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_pp_ocr_demo.sh"

# 更新模型配置文件中的路径
# for mud_file in "${RESOLVED_PKG_DIR}/configs/model_specs/"*.mud; do
#   if [ -f "$mud_file" ]; then
#     # 将 ../../models/ 替换为 ../../third_party/cv184x/models/
#     sed -i 's|model = \.\./\.\./models/|model = ../../../third_party/cv184x/models/|g' "$mud_file"
#   fi
# done
# echo "Updated model paths in .mud files"

# Runtime packages store project models under models/cv184x, including the
# multi-file ASR/KWS bundles whose extra fields use the same relative layout.
# for mud_file in "${RESOLVED_PKG_DIR}/configs/model_specs/"*.mud; do
#   if [ -f "$mud_file" ]; then
#     sed -i 's|../../../third_party/cv184x/models/|../../models/|g' "$mud_file"
#   fi
# done

tar cf - -C "$(dirname "${RESOLVED_PKG_DIR}")" "$(basename "${RESOLVED_PKG_DIR}")" | gzip -9 > "${RESOLVED_PKG_DIR}.tar.gz"
echo "TDL_APP_PROFILE=${TDL_APP_PROFILE}"
echo "TDL_APP_THIRD_PARTY_DIR=${THIRD_PARTY_DIR}"
echo "Install dir: ${RESOLVED_INSTALL_DIR}"
echo "Package dir: ${RESOLVED_PKG_DIR}"
echo "Package: ${RESOLVED_PKG_DIR}.tar.gz"
