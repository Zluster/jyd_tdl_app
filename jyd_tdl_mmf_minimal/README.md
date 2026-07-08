# jyd_tdl_mmf_minimal

This directory is a standalone C-style MMF baseline extracted from the current dual-OS media work.

It is intentionally kept separate from `jyd_tdl_app` so it can later be moved into its own repository or SDK package.

## Source State

- source repo: `/home/jyd/zwz/sophpi/jyd_tdl_app`
- source branch: `dual_benchmark`
- source commit: `7aad3908b0d2703d496335c2f9f967c5ac294914`

## Baseline

This baseline uses `mmf_*` naming everywhere and does not keep the old `jyd_mmf_*` prefix.

Current public modules:

- `system`
- `camera`
- `audio`
- `audio_3a`
- `audio_codec`
- `display`
- `jpg`
- `touch`
- `jpg_http`

## Build

This directory has its own `CMakeLists.txt`.

```bash
cd /home/jyd/zwz/sophpi/jyd_tdl_app/jyd_tdl_mmf_minimal
rm -rf build
cmake -S . -B build
cmake --build build
```

The backend now calls the CV184x dual-OS CVI media stack directly through module implementations under `src/camera`, `src/display`, `src/jpeg`, `src/http`, `src/audio`, and shared helpers under `src/core`. It no longer includes or links the `tdl_app` media wrapper layer; extraction still needs the CV184x dual-OS SDK headers/libs and the vendor `ini.c` source.

## CV184X Build And Package

```bash
cd /home/jyd/zwz/sophpi/jyd_tdl_app
BUILD_DIR=/tmp/jyd_tdl_app_mmf_build \
INSTALL_DIR=/tmp/jyd_tdl_app_mmf_install \
TDL_APP_MEDIA_MINIMAL=ON \
bash ./scripts/build_cv184x.sh

INSTALL_DIR=/tmp/jyd_tdl_app_mmf_install \
PKG_DIR=/tmp/jyd_tdl_app_mmf_pkg \
bash ./scripts/package_runtime.sh
tar -C /tmp -czf /tmp/jyd_tdl_app_mmf_pkg.tar.gz jyd_tdl_app_mmf_pkg
```

Deploy `/tmp/jyd_tdl_app_mmf_pkg.tar.gz` to the board, then run from the extracted package:

```bash
. ./env.sh
./bin/mmf_smoke_test list
./bin/mmf_smoke_test audio-read 1
./bin/mmf_smoke_test audio-read 2
./bin/mmf_smoke_test audio-control
./bin/mmf_smoke_test audio-codec
./bin/mmf_smoke_test audio-loopback 1 50
./bin/mmf_smoke_test jpg-roundtrip main
./bin/mmf_smoke_test camera-control
./bin/mmf_smoke_test display-frame screen
./bin/mmf_smoke_test display-control
./bin/mmf_smoke_test http-snapshot 18080
./bin/mmf_smoke_test http-stream 18081
./bin/mmf_smoke_test http-push 18082
./bin/mmf_smoke_test reentry 10
./bin/mmf_smoke_test stress 50
```

`camera-control` uses the small-core `CVI_MSG` ISP 3A proxy. Use it with a yoc image that contains `MSG_CMD_ISP_SOPHPI_3A_PROXY` setter support.

## Complex Example Apps

`mmf_av_monitor_app` is a video/display/HTTP integration app. It binds live video to VO, starts a screen-matching HTTP JPEG stream, samples `main` and `ai` frames once per second, and saves periodic snapshots.

```bash
. ./env.sh
./bin/mmf_av_monitor_app 60 18090 /tmp/mmf_av_monitor
```

Expected board-side behavior:

- screen shows the live camera path
- `http://<board-ip>:18090/snapshot.jpg` returns the current screen JPEG
- `http://<board-ip>:18090/stream.mjpg` keeps streaming JPEG frames
- `/tmp/mmf_av_monitor` receives periodic `main_*.jpg` and `ai_*.jpg`

`mmf_audio_full_duplex_app` is an audio capture/playback/3A/codec integration app. It opens AI and AO at the requested sample rate and channel count, optionally enables capture 3A, optionally loops captured chunks to AO, and optionally runs G711A encode/decode per chunk.

```bash
. ./env.sh
./bin/mmf_audio_full_duplex_app 30 16000 1 32 1 1 1
./bin/mmf_audio_full_duplex_app 30 16000 2 32 1 1 1
```

Arguments are `duration_s sample_rate channels volume enable_3a loopback codec`. Expected behavior is continuous per-second stats with increasing `read`, `write`, `enc`, and `dec` counters, audible loopback when `loopback=1`, and no increasing `errors`.

## Layout

- `include/mmf`: public headers
- `src`: placeholder implementations and future real implementations
- `docs`: design and migration notes
- `examples`: minimal usage samples

## Resource Ownership

- camera frames acquired by `mmf_camera_get_frame` must be released by `mmf_camera_put_frame`
- JPEG encoder and decoder sessions own their VENC/VDEC channel while open
- HTTP streaming should own or reserve its JPEG path while running
- audio capture and playback are independent sessions and may be open at the same time
- 3A configuration is attached to capture, with playback optionally acting as the AEC reference

## Current Objective

1. Stabilize the public API surface first.
2. Keep names short and uniform.
3. Make camera, audio, display, touch, jpg, and jpg_http independent modules.
4. Keep small-core control paths narrow: expose service APIs, not extra CLI/debug commands.
