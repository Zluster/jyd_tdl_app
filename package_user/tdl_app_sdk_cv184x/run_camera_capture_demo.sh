#!/bin/sh
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
. "${DIR}/env.sh"
exec "${DIR}/bin/tdl_camera_capture_demo" "$@"
