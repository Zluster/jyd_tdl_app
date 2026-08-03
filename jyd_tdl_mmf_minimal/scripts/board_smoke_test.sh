#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="${ROOT}/bin/mmf_smoke_test"
MODE="${1:-basic}"
LOG_DIR="${2:-/tmp/mmf_smoke_logs}"
STAMP=$(date +%Y%m%d_%H%M%S 2>/dev/null || echo now)
LOG="${LOG_DIR}/mmf_${MODE}_${STAMP}.log"

mkdir -p "${LOG_DIR}"

if [ -f "${ROOT}/env.sh" ]; then
  . "${ROOT}/env.sh"
fi

run_case() {
  name="$1"
  shift
  echo "===== ${name}: $*" | tee -a "${LOG}"
  if "$@" 2>&1 | tee -a "${LOG}"; then
    echo "===== ${name}: PASS" | tee -a "${LOG}"
  else
    ret=$?
    echo "===== ${name}: FAIL ret=${ret}" | tee -a "${LOG}"
    return "${ret}"
  fi
}

usage() {
  cat <<EOF
usage: $0 [basic|media|control|audio|stress|all] [log_dir]

basic   : list outputs and read mono/stereo audio once
media   : jpg encode/decode, display frame, HTTP snapshot/stream/push
control : camera ISP/control path and display bind/snapshot/clear
audio   : audio control, audio codec, mono loopback
apps    : run integrated AV monitor and audio full-duplex examples
stress  : reentry and stress loops
all     : run every group
EOF
}

case "${MODE}" in
  basic)
    run_case system "${BIN}" system
    run_case list "${BIN}" list
    run_case audio_read_mono "${BIN}" audio-read 1
    run_case audio_read_stereo "${BIN}" audio-read 2
    ;;
  media)
    run_case jpg_roundtrip_main "${BIN}" jpg-roundtrip main
    run_case display_frame_screen "${BIN}" display-frame screen
    run_case http_snapshot "${BIN}" http-snapshot 18080
    run_case http_stream "${BIN}" http-stream 18081
    run_case http_push "${BIN}" http-push 18082
    run_case http_publish_frame "${BIN}" http-publish-frame 18083
    ;;
  control)
    run_case camera_control "${BIN}" camera-control
    run_case display_control "${BIN}" display-control
    ;;
  audio)
    run_case audio_3a_global "${BIN}" audio-3a-global
    run_case audio_control "${BIN}" audio-control
    run_case audio_codec "${BIN}" audio-codec
    run_case audio_loopback_mono "${BIN}" audio-loopback 1 50
    ;;
  apps)
    run_case av_monitor "${ROOT}/bin/mmf_av_monitor_app" 15 18090 /tmp/mmf_av_monitor
    run_case audio_full_duplex_mono "${ROOT}/bin/mmf_audio_full_duplex_app" 10 16000 1 32 1 1 1
    run_case audio_full_duplex_stereo "${ROOT}/bin/mmf_audio_full_duplex_app" 10 16000 2 32 1 1 1
    ;;
  stress)
    run_case reentry "${BIN}" reentry 10
    run_case stress "${BIN}" stress 50
    ;;
  all)
    "$0" basic "${LOG_DIR}"
    "$0" media "${LOG_DIR}"
    "$0" control "${LOG_DIR}"
    "$0" audio "${LOG_DIR}"
    "$0" apps "${LOG_DIR}"
    "$0" stress "${LOG_DIR}"
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac

echo "log: ${LOG}"
