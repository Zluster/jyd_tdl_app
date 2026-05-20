# tdl_app_sdk

`tdl_app_sdk` 是面向 CV184X 平台的 C++ 应用侧封装层。它把底层 `tdl_sdk`、`CVI_MPI`、模型描述、媒体图搭建、算法后处理和板级差异收口成一组更稳定的上层接口，目的是让业务代码尽量少直接依赖厂商细节。

## 项目目标

- 给上层应用提供稳定的 C++ API。
- 把模型加载、推理、后处理封装到算法类内部。
- 同时支持单张图片、相机原生帧和板端媒体图输入。
- 让工程既能在 SDK 主机交叉编译，也能在板子上原生编译。
- 保持媒体模块、算法模块、输出模块解耦，便于后续扩展 RTSP、OSD、多路流和多阶段 pipeline。

## 当前重点能力

- 检测：`YOLOv5`、`YOLOv8`、`SCRFD`
- 分类：`Classifier`
- 特征：`FeatureExtractor`
- 人脸属性：`FaceAttributeClassifier`
- 车牌识别：`PlateRecognizer`
- 关键点、语义分割、实例分割、车道线、VAD：通过 `tdl_sdk` 封装
- 媒体输入：图片、`Camera`、`FrameSource`、`VIDEO_FRAME_INFO_S`
- 媒体输出：OSD、RTSP、编码、VO、图形图层、抓图
- 音频：AI/AO 录制播放、基础 AIO 验证

## 当前依赖策略

- 平台专用库继续使用 `third_party/cv184x/lib`
  - 例如 `libtdl_core.so`、`libbmrt.so`、`libbmlib.so`、`libsys.so`
- 通用库优先使用系统库
  - `OpenCV`
  - `zlib`
  - `tinyalsa`
- 板端运行算法 demo 时，需要可用的 `firmware/libbm1688_kernel_module.so`

## 目录说明

```text
tdl_app_sdk/
  apps/                         demo 和测试程序
  cmake/                        CMake 配置与平台依赖收敛
  configs/                      模型描述、传感器配置
  docs/                         设计、开发、板端操作文档
  include/tdl_app/              对外公开头文件
  scripts/                      构建、打包、板端初始化脚本
  src/algorithm/                算法运行时、预处理、后处理
  src/media/                    媒体、相机、编码、显示、RTSP
  src/media/private/            板级相关细节
  third_party/cv184x/           CV184X 平台依赖
```

## 常用文档入口

- [文档总览](docs/README.md)
- [架构说明](docs/ARCHITECTURE.md)
- [算法说明](docs/ALGORITHMS.md)
- [开发指南](docs/DEV_GUIDE.md)
- [开发指南（简版）](docs/DEV_GUIDE_CN.md)
- [MMF 与媒体图](docs/MMF.md)
- [绑定设计](docs/BINDINGS.md)
- [双核媒体控制](docs/DUAL_CORE.md)
- [API 与 demo 对照](docs/ALG_INFO.md)
- [板端原生编译与测试指南](docs/board_native_build_guide_zh.md)

## 推荐使用方式

### 1. 单张图片检测

```cpp
#include "tdl_app/tdl_app.hpp"

std::string error;
tdl_app::Detector detector(
    tdl_app::ModelSessionConfig::fromSpec(
        "./configs/model_specs/yolov5s_det_coco80.ini",
        "./firmware/libbm1688_kernel_module.so",
        "./models",
        "YOLOV5"),
    &error);

tdl_app::InferOptions options = tdl_app::InferOptions::detection(0.25f, 0.45f);
tdl_app::AlgorithmResult result = detector("./test.jpg", options, &error);
```

### 2. 相机原生帧检测

```cpp
VIDEO_FRAME_INFO_S frame = ...;
tdl_app::AlgorithmResult result = detector(frame, options, &error);
```

仍然兼容旧接口：

- `load(...)`
- `run(...)`
- `runFrame(...)`
- `detectFrame(...)`

## 新设备从 clone 到验证的完整流程

以下流程适用于一台新的板子，仓库目录假设为 `/mnt/git/jyd_tdl_app`。

### 1. 安装系统通用依赖

```sh
apk add opencv-dev tinyalsa-dev zlib-dev
```

### 2. clone 仓库

```sh
git clone https://github.com/Zluster/jyd_tdl_app.git /mnt/git/jyd_tdl_app
cd /mnt/git/jyd_tdl_app
```

### 3. 初始化板端环境

```sh
sh scripts/setup_board_native_env.sh
```

这个脚本会：

- 创建 `models -> third_party/cv184x/models`
- 创建 `firmware -> third_party/cv184x/firmware`
- 补系统 OpenCV 兼容软链接

### 4. 板端原生编译

```sh
cmake -S /mnt/git/jyd_tdl_app \
  -B /mnt/sd/tdl_build_sys \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /mnt/sd/tdl_build_sys --target tdl_classify_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_detect_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_camera_capture_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_audio_aio_demo -j1
```

### 5. 加载运行环境

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

也可以直接使用包装脚本：

```sh
sh scripts/run_with_board_env.sh /mnt/sd/tdl_build_sys/tdl_classify_demo ...
```

### 6. 算法分类验证

```sh
/mnt/sd/tdl_build_sys/tdl_classify_demo \
  --image /mnt/sd/dog.jpg \
  --model-spec /mnt/git/jyd_tdl_app/configs/model_specs/cls_hand_gesture.ini \
  --firmware /mnt/git/jyd_tdl_app/firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/dog_cls_syslibs.jpg
```

### 7. 相机抓图验证

当前已验证可工作的相机配置是：

- sensor ini：`configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
- backend：`vpss`

```sh
/mnt/sd/tdl_build_sys/tdl_camera_capture_demo \
  --use-sensor-media \
  --sensor-ini /mnt/git/jyd_tdl_app/configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini \
  --backend vpss \
  --width 1920 \
  --height 1080 \
  --timeout-ms 4000 \
  --frames 1 \
  --output /mnt/sd/fullstack_probe.jpg
```

说明：

- 之前抓不到图，核心原因是 ini 配置不匹配，不是 demo 读帧逻辑有问题。
- 目前这块板子上 `--backend vi` 仍然会超时，`--backend vpss` 已验证可用。

### 8. 音频录制与播放验证

录音：

```sh
/mnt/sd/tdl_build_sys/tdl_audio_aio_demo \
  --mode record \
  --seconds 5 \
  --ai-volume 24 \
  --output /mnt/sd/test_ai_16k_mono.pcm
```

播放：

```sh
/mnt/sd/tdl_build_sys/tdl_audio_aio_demo \
  --mode play \
  --input /mnt/sd/test_ai_16k_mono.pcm \
  --ao-volume 24
```

如果 PCM 文件大小正常但仍然没有声音，优先检查：

- 板端扬声器、功放、耳机路径是否真的接通
- DAC 输出音量是否被其他流程覆盖
- 当前板卡音频输出是否接到了外放路径

## 关于 `--firmware`

很多 BMRuntime 模型在板端运行时需要显式指定：

```sh
--firmware ./firmware/libbm1688_kernel_module.so
```

原因不是“每帧都要重新加载 firmware”，而是模型会话初始化时需要告诉 BMRuntime 使用哪套 TPU kernel 模块。这个路径通常在 `Detector`、`Classifier` 的 `load/open` 阶段通过环境变量传给 BMRuntime，一次初始化后，后续每帧只是复用同一个 runtime 句柄。

## 当前建议

- 相机流优先直接给 `VIDEO_FRAME_INFO_S`。
- 单张图保留图片路径接口，便于离线验证。
- 多模型同时运行时，每个模型实例独立持有自己的 runtime，不要在业务层反复创建和销毁。
- 板端初始化后，优先使用 `env.sh` 或 `scripts/run_with_board_env.sh`，避免遗漏 `LD_LIBRARY_PATH` 和 `BMRUNTIME_USING_FIRMWARE`。
