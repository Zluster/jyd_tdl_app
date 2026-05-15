# tdl_app_sdk

`tdl_app_sdk` is a portable C++ wrapper project for CV184X `tdl_sdk`.
It is intentionally separate from the Sophpi SDK tree so it can become the
application-facing algorithm module.

## Goals

- Provide stable C++ APIs for upper-layer applications.
- Keep TDL SDK details behind `AlgorithmEngine`.
- Support image-file validation first, then camera frame input through
  `FrameSource`/`CameraSource`.
- Build with CMake from a portable dependency bundle, not by living inside the
  original Sophpi SDK source tree.
- Use a base-class + subclass structure so each algorithm family can own
  initialization, inference, and postprocess logic separately.
- Keep input, algorithm, and output as separate modules so future camera and
  RTSP features do not leak into the algorithm core.
- Parse application-level model descriptor files, so upper layers do not need
  to depend on any vendor `model_factory.json`.

Additional design notes live in:

- [Architecture](docs/ARCHITECTURE.md)
- [Algorithms](docs/ALGORITHMS.md)
- [MMF](docs/MMF.md)

## Directory Layout

```text
tdl_app_sdk/
  apps/                         # focused CLI/demo programs, one task per entry
  cmake/                        # CMake dependency and toolchain files
  include/tdl_app/              # public API for upper layer
  scripts/                      # collect / clean / build / package scripts
  src/algorithm/                # algorithm/runtime implementation
  src/media/                    # MMF/media implementation
  src/media/private/            # board-specific sensor/ISP/VI bring-up details
  src/                          # pipeline glue and remaining top-level composition
  third_party/cv184x/           # generated dependency bundle, not hand-written
```

## Dependency Model

This project does not assume it is inside `/home/jyd/zwz/sophpi`.
Instead, it builds against a portable bundle:

```text
host-tools/
  gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf/
third_party/cv184x/
  include/                      # TDL headers
  lib/                          # libtdl_core, bmrt, bmlib, cvi_mpi libs, musl libs
  cvi_mpi/include
  cvi_mpi/lib
  opencv/include
  opencv/lib
  models/cv184x/*.bmodel
  firmware/libbm1688_kernel_module.so
```

Generate this bundle from an existing Sophpi SDK:

```sh
cd /home/jyd/zwz/sophpi/tdl_app_sdk
SOPHPI_ROOT=/home/jyd/zwz/sophpi ./scripts/collect_cv184x_deps.sh
```

After that, the `tdl_app_sdk` directory can be copied to another machine. The
new machine still needs the ARM cross toolchain to build for the board. So the
repo is independent from the Sophpi source tree, but not independent from the
CV184X vendor runtime and toolchain.

Recommended portable layout:

```text
tdl_app_sdk/
  CMakeLists.txt
  include/
  src/
  scripts/
  host-tools/                   # external bundle, ignored by git
  third_party/cv184x/           # external bundle, ignored by git
```

If `host-tools` is not inside the project, set:

```sh
export TOOLCHAIN_ROOT=/path/to/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf
```

## GitHub Readiness

If you publish this on GitHub, other people can clone it and start development
without cloning Sophpi itself, provided you ship one of these:

- a prebuilt `third_party/cv184x` bundle as a release asset
- a prebuilt `host-tools` bundle as a release asset, or documented toolchain URL
- or the `collect_cv184x_deps.sh` script plus a documented Sophpi path

That means:

- application and wrapper development: yes, the repo is self-contained
- CV184X target build/run: still needs vendor toolchain/runtime/model bundle
- normal `git clone`: only downloads source files, not ignored dependency bundles

## Build For CV184X

Recommended daily workflow:

1. Edit locally on your development machine.
2. Sync the updated `tdl_app_sdk` tree to the SDK host.
3. Build and install on the SDK host with a user-writable `BUILD_DIR` /
   `INSTALL_DIR`.
4. Package from the same install tree.
5. Copy the generated runtime directory or tarball to the board.
6. Run the required demo on the board.

Core scripts:

- `scripts/collect_cv184x_deps.sh`: collect vendor headers, libs, models, firmware
- `scripts/clean_tree.sh`: remove `build/`, `install/`, `package/`, and misplaced root-level source files
- `scripts/build_cv184x.sh`: cross-build and install into `install/cv184x/`
- `scripts/package_runtime.sh`: generate `package/tdl_app_sdk_cv184x/` and `package/tdl_app_sdk_cv184x.tar.gz`

Build outputs:

- `install/cv184x/`: staged install tree
- `install_user/cv184x/`: recommended staged install tree on shared SDK hosts
- `package/tdl_app_sdk_cv184x/`: unpacked runtime directory
- `package/tdl_app_sdk_cv184x.tar.gz`: runtime tarball for board deployment

Recommended SDK host commands:

```sh
cd /home/jyd/zwz/sophpi/tdl_app_sdk
export TDL_APP_THIRD_PARTY_DIR=/home/jyd/zwz/sophpi/tdl_app_sdk/third_party/cv184x
export BUILD_DIR=/home/jyd/zwz/sophpi/tdl_app_sdk/build_user/cv184x
export INSTALL_DIR=/home/jyd/zwz/sophpi/tdl_app_sdk/install_user/cv184x
export PKG_DIR=/home/jyd/zwz/sophpi/tdl_app_sdk/package_user/tdl_app_sdk_cv184x

bash scripts/build_cv184x.sh
bash scripts/package_runtime.sh
```

Packaging note:

- `scripts/package_runtime.sh` now prefers `${INSTALL_DIR}` when set.
- `scripts/package_runtime.sh` now prefers `${PKG_DIR}` when set.
- If `INSTALL_DIR` is not set, it prefers `install_user/cv184x/` when present,
  then falls back to `install/cv184x/`.
- If the default `package/tdl_app_sdk_cv184x/` is not writable, it falls back
  to `package_user/tdl_app_sdk_cv184x/`.
- This avoids packaging stale binaries from `install/cv184x/` after building
  into `install_user/cv184x/`.
- `package/tdl_app_sdk_cv184x.tar.gz`: board runtime bundle

If you move the project to another machine, build with:

```sh
export TOOLCHAIN_ROOT=/path/to/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf
export TDL_APP_THIRD_PARTY_DIR=/path/to/third_party/cv184x
./scripts/build_cv184x.sh
```

## Public API

Upper-layer code should include only:

```cpp
#include "tdl_app/tdl_app.hpp"
```

If you need compatibility runtimes or low-level media graph primitives, include:

```cpp
#include "tdl_app/advanced.hpp"
```

Default high-level API:

- `Detector`
- `Classifier`
- `FeatureExtractor`
- `Camera`
- `Pipeline`
- `FrameSource`
- `FrameSink`
- `ModelDescriptor`

Advanced media / board control API:

- `SensorMedia`
- `Mmf`
- `SysContext`
- `VideoBufferManager`
- `VideoBufferPool`
- `VdecChannel`
- `VencChannel`
- `OsdRegion`
- `RegionOverlay`
- `MediaSystem`
- `MediaLink`
- `ViChannel`
- `VpssGroup`
- `VoOutput`
- `RtspStreamer`

Detector example:

```cpp
tdl_app::Detector detector = tdl_app::Detector::yolov8();
std::string error;
if (!detector.load(tdl_app::ModelSessionConfig::fromSpec(
        "/mnt/sd/tdl_app_sdk_cv184x/configs/model_specs/yolov8n_det_coco80.ini",
        "/mnt/sd/tdl_app_sdk_cv184x/firmware/libbm1688_kernel_module.so",
        "/mnt/sd/tdl_app_sdk_cv184x/models"),
    &error)) {
  // handle error
}

tdl_app::AlgorithmResult result;
detector.detect("/mnt/sd/dog.jpg", 0.25f, &result, &error);
```

Classifier is similar:

```cpp
tdl_app::Classifier cls = tdl_app::Classifier::generic();
cls.load(tdl_app::ModelSessionConfig::fromSpec(
    "/mnt/sd/tdl_app_sdk_cv184x/configs/model_specs/cls_hand_gesture.ini"),
    &error);
cls.classify("/mnt/sd/image.jpg", 0.25f, &result, &error);
```

Feature extractor is similar:

```cpp
tdl_app::FeatureExtractor feature = tdl_app::FeatureExtractor::generic();
feature.load(tdl_app::ModelSessionConfig::fromSpec(
    "/mnt/sd/tdl_app_sdk_cv184x/configs/model_specs/feature_cviface.ini"),
    &error);
feature.extract("/mnt/sd/image.jpg", 0.25f, &result, &error);
```

Pipeline style:

```cpp
tdl_app::Pipeline pipeline;
pipeline.setImage("/mnt/sd/dog.jpg");
pipeline.setDetector(detector);
pipeline.setNullSink();
pipeline.open(&error);
pipeline.runOnce(0.25f, &result, &error);
```

Camera + pipeline style:

```cpp
tdl_app::Camera::Config camera_cfg =
    tdl_app::Camera::vpss(0, 0, 640, 640);

tdl_app::Pipeline pipeline;
pipeline.setCamera(camera_cfg);
pipeline.setDetector(detector);
pipeline.open(&error);
pipeline.runOnce(0.25f, &result, &error);
```

Advanced board-side camera bootstrap:

```cpp
#include "tdl_app/advanced.hpp"

tdl_app::SensorMedia::Config media_cfg =
    tdl_app::SensorMedia::fullStackSensor(
        "./configs/sensor_cfg_0x44003340.ini",
        true,   // use VPSS backend
        0, 0, 0,
        0, 0,
        640, 640, 18);

tdl_app::SensorMedia media(media_cfg);
media.open(&error);

tdl_app::Camera camera(tdl_app::Camera::vpss(0, 0, 640, 640));
camera.open(&error);
```

Advanced `VI -> 2x VPSS` graph:

```cpp
#include "tdl_app/advanced.hpp"

tdl_app::SensorMedia::Config media_cfg =
    tdl_app::SensorMedia::fullStackSensor(
        "./configs/sensor_cfg_0x44003340.ini",
        true, 0, 0, 0, 0, 0, 640, 640, 18);
media_cfg.vpss_outputs.clear();
media_cfg.vpss_outputs.push_back(
    tdl_app::SensorMedia::vpssOutput(0, 0, 640, 640, 18));
media_cfg.vpss_outputs.push_back(
    tdl_app::SensorMedia::vpssOutput(1, 0, 960, 540, 18));

tdl_app::SensorMedia media(media_cfg);
media.open(&error);

tdl_app::Camera det_cam(tdl_app::Camera::vpss(0, 0, 640, 640));
tdl_app::Camera preview_cam(tdl_app::Camera::vpss(1, 0, 960, 540));
```

Equivalent low-level graph with `Mmf`:

```cpp
tdl_app::Mmf::Config mmf_cfg;
mmf_cfg.graph.vpss.push_back(
    tdl_app::Mmf::vpssOutput(0, 0, 1920, 1080, 640, 640, 18));
mmf_cfg.graph.vpss.push_back(
    tdl_app::Mmf::vpssOutput(1, 0, 1920, 1080, 960, 540, 18));
mmf_cfg.graph.binds.push_back(tdl_app::Mmf::viToVpss(0, 0, 0, 0));
mmf_cfg.graph.binds.push_back(tdl_app::Mmf::viToVpss(0, 0, 1, 0));

tdl_app::Mmf mmf(mmf_cfg);
mmf.open(&error);
```

The same graph can now be expressed with shorter builders:

```cpp
std::vector<tdl_app::Mmf::VpssConfig> outputs;
outputs.push_back(tdl_app::Mmf::vpssOutput(0, 0, 1920, 1080, 640, 640, 18));
outputs.push_back(tdl_app::Mmf::vpssOutput(1, 0, 1920, 1080, 960, 540, 18));

tdl_app::Mmf mmf(tdl_app::Mmf::viToMultiVpss(outputs, 0, 0));
mmf.open(&error);

tdl_app::OsdRegion osd(tdl_app::OsdRegion::canvas(0, 320, 80));
osd.create(&error);
osd.attach(tdl_app::MediaChannel::venc(0), 16, 16, 0, &error);
```

The same OSD path can also be expressed directly through `Mmf::GraphConfig`:

```cpp
tdl_app::Mmf::GraphConfig graph;
graph.osd.push_back(tdl_app::OsdRegion::canvas(0, 320, 80));
graph.osd_attaches.push_back(
    tdl_app::Mmf::osdAttach(0, tdl_app::MediaChannel::venc(0), 16, 16, 0));

tdl_app::Mmf mmf(tdl_app::Mmf::mediaGraph(graph));
mmf.open(&error);
```

## Interface Design

Preferred public API:

- `Detector`
- `Classifier`
- `FeatureExtractor`
- `Pipeline`
- `Camera`
- `FrameSource`
- `FrameSink`
- `ModelDescriptor`

Compatibility / lower-level API still available:

- `advanced.hpp`
- `SensorMedia`
- `Mmf`
- `SysContext`
- `VideoBufferManager`
- `VideoBufferPool`
- `VdecChannel`
- `VencChannel`
- `OsdRegion`
- `RegionOverlay`
- `MediaSystem`
- `MediaLink`
- `ViChannel`
- `VpssGroup`
- `VoOutput`
- `RtspStreamer`
- `NnBase`
- `NnClassifier`
- `NnYolov5`
- `NnYolov8`
- `NnFeature`
- `VisionPipeline`
- `AlgorithmEngine`

The new API is intentionally thinner. The old `Nn*` and `VisionPipeline` names
remain for compatibility while the external direction moves toward a simpler
`Detector / Classifier / Pipeline` style similar to MaixCDK.

Header note:

- `pipeline.hpp`: primary public pipeline API
- `simple_pipeline.hpp`: compatibility forwarding header only

Layer split:

- `Mmf`: media-system lifecycle, VB/SYS/VPSS/bind
- `ViChannel`: explicit `VI` channel lifecycle
- `FrameReader`: direct `VI/VPSS` frame pull
- `Camera`: frame acquisition only
- `Detector` / `Classifier`: algorithm only
- `RtspStreamer`: encoded output transport only
- `Pipeline`: source + algorithm + sink composition
- `SensorMedia`: sensor/MIPI/ISP/VI/VPSS startup helper for board-side bring-up

Recommended usage patterns:

- `ImageFileSource + NnBase/NnYolov5 + NullFrameSink`
- `CameraSource + NnBase/NnYolov5 + NullFrameSink`
- `CameraSource + NnBase/NnYolov5 + future RtspFrameSink`

`NnBase` subclasses must be initialized with `EngineConfig` before inference or
before attaching them to `VisionPipeline`.

## Camera Integration Plan

The current implementation supports image files first. Camera support should be
added without changing the upper-layer result structures:

1. Implement `CameraSource` in `src/frame_source.cpp` using VI/VPSS or the
   vendor sample camera pipeline.
2. Add an `AlgorithmEngine::runFrame(...)` API that accepts native CVI frames or
   `BaseImage` wrappers.
3. Keep detection/classification/feature result types unchanged.
4. Add camera demos under `apps/`, not inside the algorithm wrapper itself.
5. Add a `FrameSink` implementation for RTSP or on-device preview.

This keeps camera capture, model inference, and application policy separated.

Current status:

- `model_descriptor_file` is supported as the portable application-level model
  description.
- Inference no longer depends on `model_factory.json`.
- `runFrameSource(...)` exists as the stable API hook for camera/stream input.
- `FrameReader` can read frames from already running `VI` or `VPSS` channels.
- `Camera` is now a thinner application wrapper built on `FrameReader`.
- `ViChannel`, `VpssGroup`, `MediaLink`, and `RegionOverlay` expose reusable
  media primitives for future graph assembly.
- `SysContext`, `VideoBufferManager`, `VideoBufferPool`, `OsdRegion`,
  `VdecChannel`, and `VencChannel` now provide dedicated low-level wrappers for
  `SYS`, `VB`, `RGN/OSD`, `VDEC`, and `VENC`.
- `runFrame(...)` accepts native `VIDEO_FRAME_INFO_S` through `Frame::native`.
- `RtspFrameSink` is available for H.264/H.265 RTSP publishing.
- `Mmf` now provides a dedicated bootstrap layer for `SYS/VB/VPSS` and optional
  `VI -> VPSS` bind.
- `Mmf` graph config now also accepts `VENC`, `VDEC`, `VO`, `OSD`, and generic
  `MediaLink` bind entries so upper layers can assemble more complete media
  paths without dropping to raw wrapper orchestration immediately.
- `Mmf` graph config now also supports `osd_attaches`, so `OSD` create and
  attach/layout can be kept in the same graph description.
- `SensorMedia` now provides a dedicated board-side media bootstrap module with:
  `FullStack` and `AttachExisting` startup modes.
- Full sensor / ISP / VI board-specific startup remains isolated from the
  algorithm wrappers and lives under `src/media/private/`.
- `NnYolov5` custom runtime now accepts native `VIDEO_FRAME_INFO_S` camera
  frames in NV12 / NV21 / YUV400 format, so `tdl_camera_detect_demo` no longer
  needs to save camera frames as temporary image files first.

## Demo Layout

Smaller demos are preferred over one large multi-mode binary:

- `tdl_yolov5_demo`: image + YOLOv5 detector
- `tdl_yolov8_demo`: image + YOLOv8 detector
- `tdl_detect_demo`: generic image detector driven by `model-spec`
- `tdl_classify_demo`: image + classifier
- `tdl_feature_demo`: image + feature extractor
- `tdl_camera_capture_demo`: camera snapshot only
- `tdl_camera_detect_demo`: camera + detector
- `tdl_camera_classify_demo`: camera + classifier
- `tdl_camera_feature_demo`: camera + feature extractor
- `tdl_camera_rtsp_demo`: camera + detector + RTSP
- `tdl_multi_vpss_demo`: sensor/VI fan-out to multiple VPSS outputs
- `tdl_media_smoke_demo`: low-level media module smoke test
- `tdl_osd_demo`: isolated OSD create/destroy smoke test
- `tdl_vdec_demo`: low-level VDEC stream decode smoke test

The old catch-all demos have been removed to keep the demo surface
single-purpose and easier to maintain.

## Media Module Smoke Test

Low-level media wrappers now include:

- `SysContext`
- `VideoBufferManager`
- `VideoBufferPool`
- `VencChannel`
- `VdecChannel`
- `OsdRegion`
- `ViChannel`
- `VpssGroup`
- `MediaLink`
- `VoOutput`

Convenience builders are available for common paths:

- `ModelSessionConfig::fromSpec(...)`
- `Detector::yolov5()` / `Detector::yolov8()`
- `Classifier::generic()` / `FeatureExtractor::generic()`
- `MediaChannel::vi(...)`, `vpss(...)`, `venc(...)`, `vo(...)`, `vdec(...)`
- `Mmf::singleVpss(...)` and `Mmf::viToMultiVpss(...)`
- `Mmf::osdAttach(...)`
- `VencChannel::h264(...)` / `h265(...)`
- `VdecChannel::h264(...)` / `h265(...)` / `jpeg(...)` / `mjpeg(...)`
- `RtspStreamer::h264(...)` / `h265(...)`

Smoke verification command:

```sh
./env.sh

./bin/tdl_media_smoke_demo \
  --width 640 --height 640 \
  --venc-channel 2 \
  --vdec-channel 2 \
  --osd-handle 2
```

Isolated OSD verification command:

```sh
./env.sh

./bin/tdl_osd_demo \
  --handle 100 \
  --width 300 \
  --height 200 \
  --pixel-format 4 \
  --canvas-count 2
```

VDEC verification command:

```sh
./env.sh

./bin/tdl_vdec_demo \
  --input /mnt/sd/dog.jpg \
  --codec jpeg \
  --channel 2 \
  --width 1920 \
  --height 1080 \
  --timeout-ms 1000 \
  --max-reads 10
```

Current status:

- `SysContext`: board verified
- `VideoBufferManager` / `VideoBufferPool`: board verified
- `VencChannel`: board verified
- `tdl_yolov5_demo`: board verified on image input
- `tdl_yolov8_demo`: board verified on image input, without `model_factory.json`
- `tdl_camera_capture_demo --backend vpss`: board verified against an existing
  `ipcamera` media graph
- `tdl_camera_detect_demo`: board verified against an existing board media graph
- `tdl_camera_rtsp_demo`: board verified against an existing board media graph
- `VdecChannel`: board verified for create/start/query, and user-pool attach is
  now board verified
- `VdecChannel` JPEG decode: still under convergence on the current board image;
  `SendStream` succeeds, and `tdl_vdec_demo` now trims JPEG input to the parsed
  frame boundaries before sending; final board decode verification is still
  pending
- `OsdRegion`: wrapper is implemented, and `tdl_osd_demo` now isolates the OSD
  path from `VENC/VDEC`; final board verification of `CVI_RGN_Create` is still
  pending on the active board image
- `SensorMedia` full-stack bring-up: still under convergence; direct sensor
  startup does not yet match the working `ipcamera` graph, while
  `AttachExisting` / existing-graph capture already works

## Camera Demos

Basic frame read:

```sh
./env.sh

./bin/tdl_camera_capture_demo \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 10 \
  --output frame.jpg
```

With MMF bootstrap:

```sh
./env.sh

./bin/tdl_camera_capture_demo \
  --use-mmf \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 10 \
  --output frame.jpg
```

Camera + detection:

```sh
./env.sh

./bin/tdl_camera_detect_demo \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 100 \
  --model-spec ./configs/model_specs/yolov5s_det_coco80.ini \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25
```

Camera + classification:

```sh
./env.sh

./bin/tdl_camera_classify_demo \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 10 \
  --model-spec ./configs/model_specs/cls_hand_gesture.ini \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --top-k 5
```

Camera + feature extraction:

```sh
./env.sh

./bin/tdl_camera_feature_demo \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 10 \
  --model-spec ./configs/model_specs/feature_cviface.ini \
  --firmware ./firmware/libbm1688_kernel_module.so
```

Camera + detection + RTSP:

```sh
./env.sh

./bin/tdl_camera_rtsp_demo \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 0 \
  --venc-channel 0 \
  --model-spec ./configs/model_specs/yolov5s_det_coco80.ini \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25
```

Notes:

- `--frames 0` means loop until interrupted.
- `--use-mmf` currently bootstraps `SYS/VB/VPSS` and optional `VI -> VPSS`
  bind.
- If your board already starts `VI/VPSS` elsewhere, you can omit `--use-mmf`
  and use `Camera` only.

With board-side sensor bring-up:

```sh
./env.sh

./bin/tdl_camera_capture_demo \
  --use-sensor-media \
  --sensor-ini /mnt/data/sensor_cfg.ini \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 10 \
  --output frame.jpg
```

Sensor ini note:

- Keep `rst_port`, `rst_pin`, and `rst_pol` in the sensor ini for sensors that
  need an explicit reset toggle, including the `0x44003340` / `CV2003` board.
- `SensorMedia` now reapplies these critical ini fields on top of
  `CVI_SNS_GetConfigInfo()` because the vendor parser does not always carry the
  reset GPIO values through on its own.
- `tdl_camera_capture_demo` now keeps `reuse_existing_sys` /
  `reuse_existing_vb` enabled by default during full-stack startup. Use
  `--attach-existing` only when you want to skip full bring-up and attach to an
  already running board media graph.

With board-side `ipcamera` helper bring-up:

```sh
./env.sh

./bin/tdl_camera_capture_demo \
  --use-ipcamera-helper \
  --ipcamera-bin /mnt/sd/install/ipcamera \
  --ipcamera-ini /mnt/sd/install/cv1842hp_wevb_cv2003.ini \
  --backend vpss \
  --group 0 --channel 0 \
  --frames 1 \
  --output frame.jpg
```

Additional sensor ini presets bundled for quick board-side comparison:

- `configs/sensor_cfg_0x44003340.ini`
- `configs/sensor_cfg_0x44003340_addr54_mclk1.ini`
- `configs/sensor_cfg_0x44003300_addr53_mclk1.ini`
- `configs/sensor_cfg_cv2003_switch_high_addr54_mclk1.ini`
- `configs/sensor_cfg_cv2003_switch_high_addr54_mclk1_lane21.ini`
- `configs/sensor_cfg_cv2003_switch_master_addr53_mclk1.ini`

Attach to an existing board media graph:

```sh
./env.sh

./bin/tdl_camera_capture_demo \
  --use-sensor-media \
  --attach-existing \
  --sensor-ini /mnt/data/sensor_cfg.ini \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 10 \
  --output frame.jpg
```

## Build, Package, And Deploy

### 1. Collect vendor dependencies

Run once after the Sophpi dependency layout changes:

```sh
cd /home/jyd/zwz/sophpi/tdl_app_sdk
SOPHPI_ROOT=/home/jyd/zwz/sophpi ./scripts/collect_cv184x_deps.sh
```

This fills `third_party/cv184x/` with the runtime libraries, OpenCV, models,
firmware, and vendor headers required by the portable build.

### 2. Sync latest source to the SDK host

Recommended from the Windows workspace:

First, on the SDK host parent directory, remove the old source tree if you want
to avoid stale deleted files being left behind:

```bash
cd /home/jyd/zwz/sophpi
rm -rf tdl_app_sdk
```

Then upload the current tree:

```powershell
scp -r .\tdl_app_sdk jyd@192.168.3.17:/home/jyd/zwz/sophpi/
```

If you are already editing directly on the SDK host, skip this upload step.

### 3. Build inside the SDK Docker container

```bash
cd /home/jyd/zwz/sophpi
sudo docker exec -it cvitek-linux /bin/bash
cd /home/jyd/zwz/sophpi/tdl_app_sdk
./scripts/clean_tree.sh
./scripts/build_cv184x.sh
./scripts/package_runtime.sh
```

Before building, verify that this is the latest synced tree:

```bash
cd /home/jyd/zwz/sophpi/tdl_app_sdk
ls scripts/clean_tree.sh
find apps -maxdepth 1 \( -name 'tdl_camera_demo.cpp' -o -name 'tdl_image_demo.cpp' \)
```

Expected:

- `scripts/clean_tree.sh` exists
- the `find` command prints nothing

If `clean_tree.sh` is missing, or old demo files still exist, you are in a
stale source tree and must re-upload the current `tdl_app_sdk` directory before
continuing.

What each step does:

- `clean_tree.sh`: removes `build/`, `install/`, `package/`, and misplaced root-level source files such as accidental `./nn_base.hpp` or `./nn_yolov5.cpp`
- `build_cv184x.sh`: configures and cross-builds with CMake/Ninja, then installs into `install/cv184x/`
- `package_runtime.sh`: assembles `package/tdl_app_sdk_cv184x/` and `package/tdl_app_sdk_cv184x.tar.gz`

If the toolchain or dependency bundle lives elsewhere:

```sh
export TOOLCHAIN_ROOT=/path/to/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf
export TDL_APP_THIRD_PARTY_DIR=/path/to/third_party/cv184x
./scripts/build_cv184x.sh
./scripts/package_runtime.sh
```

### 4. Copy the package to the board

Recommended from the SDK host:

```bash
scp package/tdl_app_sdk_cv184x.tar.gz root@10.0.0.1:/mnt/sd/
```

### 5. Unpack and run on the board

```sh
cd /mnt/sd
tar -xzf tdl_app_sdk_cv184x.tar.gz
cd tdl_app_sdk_cv184x
. ./env.sh
```

Packaged wrapper scripts:

- `run_detect_demo.sh`
- `run_yolov5_demo.sh`
- `run_yolov8_demo.sh`
- `run_classify_demo.sh`
- `run_feature_demo.sh`
- `run_camera_capture_demo.sh`
- `run_camera_detect_demo.sh`
- `run_camera_classify_demo.sh`
- `run_camera_feature_demo.sh`
- `run_camera_rtsp_demo.sh`
- `run_multi_vpss_demo.sh`
- `run_media_smoke_demo.sh`
- `run_vdec_demo.sh`
- `run_osd_demo.sh`

Package layout note:

- unpacked runtime directory: `package/tdl_app_sdk_cv184x/`
- tarball: `package/tdl_app_sdk_cv184x.tar.gz`
- binaries live under `package/tdl_app_sdk_cv184x/bin/`, not `package/bin/`

Quick verification on the SDK host:

```bash
cd /home/jyd/zwz/sophpi/tdl_app_sdk
strings package/tdl_app_sdk_cv184x/bin/tdl_camera_capture_demo | grep "camera config:"
```

Image detection example:

```sh
./run_detect_demo.sh \
  --model-spec ./configs/model_specs/yolov8n_det_coco80.ini \
  --image /mnt/sd/dog.jpg \
  --threshold 0.25
```

Camera snapshot example:

```sh
./run_camera_capture_demo.sh \
  --use-sensor-media \
  --sensor-ini ./configs/sensor_cfg_0x44003340_addr54_mclk1.ini \
  --group 0 --pipe 0 --channel 0 \
  --width 640 --height 640 \
  --frames 1 \
  --output /mnt/sd/frame.jpg
```

Multi-VPSS example:

```sh
./run_multi_vpss_demo.sh \
  --sensor-ini ./configs/sensor_cfg_0x44003340.ini \
  --group0 0 --channel0 0 --width0 640 --height0 640 \
  --group1 1 --channel1 0 --width1 960 --height1 540 \
  --output0 /mnt/sd/frame_g0.jpg \
  --output1 /mnt/sd/frame_g1.jpg
```

The generated package is self-contained for board-side runtime, but a fresh
source clone still needs the toolchain bundle and `third_party/cv184x/` to
compile.

## Current Scope

Implemented:

- Classification
- Object detection
- Feature extraction
- Custom YOLOv5 detection through `NnYolov5 + model_spec`
- Custom YOLOv8 detection through `NnYolov8 + model_spec`
- Camera + detection, classification, and feature extraction demos
- Media primitives for `SYS/VB/VI/VPSS/BIND/RGN/VENC/VDEC/VO/RTSP`
- Builder helpers for common `VI -> VPSS`, multi-VPSS, OSD, VENC, VDEC, and
  RTSP paths
- Sensor-backed bring-up helper for camera validation

Not yet implemented as closed-loop APIs:

- Face / pose / OCR dedicated wrappers on top of the custom runtime layer
- Face database enrollment/matching
- MOT/counting
- SOT tracking
- KWS/ASR audio pipelines
