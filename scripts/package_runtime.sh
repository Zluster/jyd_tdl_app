#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
THIRD_PARTY_DIR="${TDL_APP_THIRD_PARTY_DIR:-${PROJECT_ROOT}/third_party/cv184x}"
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

rm -rf "${RESOLVED_PKG_DIR}"
mkdir -p "${RESOLVED_PKG_DIR}/bin" "${RESOLVED_PKG_DIR}/lib" "${RESOLVED_PKG_DIR}/models" "${RESOLVED_PKG_DIR}/configs" "${RESOLVED_PKG_DIR}/firmware"

cp -a "${RESOLVED_INSTALL_DIR}/bin/." "${RESOLVED_PKG_DIR}/bin/" 2>/dev/null || true
cp -a "${RESOLVED_INSTALL_DIR}/lib/." "${RESOLVED_PKG_DIR}/lib/" 2>/dev/null || true
cp -a "${RESOLVED_INSTALL_DIR}/configs/." "${RESOLVED_PKG_DIR}/configs/" 2>/dev/null || true
cp -a "${THIRD_PARTY_DIR}/lib/." "${RESOLVED_PKG_DIR}/lib/"
cp -a "${THIRD_PARTY_DIR}/opencv/lib/." "${RESOLVED_PKG_DIR}/lib/"
cp -a "${THIRD_PARTY_DIR}/models/." "${RESOLVED_PKG_DIR}/models/"
cp -a "${THIRD_PARTY_DIR}/firmware/." "${RESOLVED_PKG_DIR}/firmware/" 2>/dev/null || true

cat > "${RESOLVED_PKG_DIR}/env.sh" <<'EOF'
#!/bin/sh
DIR=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="${DIR}/lib:${LD_LIBRARY_PATH:-}"
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

cat > "${RESOLVED_PKG_DIR}/run_camera_rtsp_demo.sh" <<'EOF'
#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_camera_rtsp_demo" "$@"
EOF
chmod +x "${RESOLVED_PKG_DIR}/run_camera_rtsp_demo.sh"

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

tar -czf "${RESOLVED_PKG_DIR}.tar.gz" -C "$(dirname "${RESOLVED_PKG_DIR}")" "$(basename "${RESOLVED_PKG_DIR}")"
echo "Install dir: ${RESOLVED_INSTALL_DIR}"
echo "Package dir: ${RESOLVED_PKG_DIR}"
echo "Package: ${RESOLVED_PKG_DIR}.tar.gz"
