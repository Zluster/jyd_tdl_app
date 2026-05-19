# tdl_app_sdk

`tdl_app_sdk` 是面向 CV184X 的 C++ 应用侧封装层。它把底层 `tdl_sdk`、`CVI_MPI`、模型描述、媒体图搭建和算法后处理收口成一套更稳定的上层接口，目标是让业务代码少碰厂商细节。

## 目标

- 给上层应用提供稳定的 C++ API。
- 把模型加载、推理、后处理封装到算法类里。
- 同时支持单张图片和相机原生帧输入。
- 让工程可以脱离 Sophpi 源码树单独开发、单独打包。
- 保持媒体模块、算法模块、输出模块解耦，便于后续扩展 RTSP、OSD、多路流和多阶段 pipeline。

## 当前重点能力

- 检测：`YOLOv5`、`YOLOv8`、`SCRFD`
- 分类：`Classifier`
- 特征：`FeatureExtractor`
- 人脸属性：`FaceAttributeClassifier`
- 车牌识别：`PlateRecognizer`
- 关键点 / 语义分割 / 实例分割 / 车道线 / VAD：走 `tdl_sdk` 包装
- 媒体输入：图片、`Camera`、`FrameSource`
- 媒体输出：OSD、RTSP、编码、VO、图形图层

## 新的 Detector 使用方式

现在推荐直接“构造后调用”：

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

if (!detector.initialized()) {
  // error
}

tdl_app::InferOptions options = tdl_app::InferOptions::detection(0.25f, 0.45f);
tdl_app::AlgorithmResult image_result = detector("./test.jpg", options, &error);
```

相机原生帧也可以直接调用：

```cpp
VIDEO_FRAME_INFO_S frame = ...;
tdl_app::AlgorithmResult frame_result = detector(frame, options, &error);
```

仍然兼容旧接口：

- `load(...)`
- `run(...)`
- `runFrame(...)`
- `detectFrame(...)`

## YOLOv5 当前输入能力

`YOLOv5` 现在同时支持：

- JPG / PNG 等图片路径输入
- `Frame`
- 直接传入 `VIDEO_FRAME_INFO_S`

当前支持的原生帧格式：

- `PIXEL_FORMAT_RGB_888`
- `PIXEL_FORMAT_BGR_888`
- `PIXEL_FORMAT_RGB_888_PLANAR`
- `PIXEL_FORMAT_BGR_888_PLANAR`
- `PIXEL_FORMAT_NV12`
- `PIXEL_FORMAT_NV21`
- `PIXEL_FORMAT_YUV_400`

这意味着摄像头如果已经直接输出 RGB/BGR，就不需要先转 JPG 再推理，也不需要上层自己再包一层 `Frame`。

## 目录

```text
tdl_app_sdk/
  apps/                 demo 和测试程序
  include/tdl_app/      对外公开头文件
  src/algorithm/        算法运行时、预处理、后处理
  src/media/            媒体、相机、编码、显示、RTSP
  src/media/private/    板级相关细节
  configs/              模型描述、传感器配置
  docs/                 设计与开发文档
  third_party/cv184x/   依赖包
```

## 构建与打包

典型流程：

1. 在开发机修改源码。
2. 同步到 SDK 主机。
3. 在 SDK 主机交叉编译。
4. 打包生成板端运行目录。
5. 拷到板端运行 demo 验证。

常用脚本：

- `scripts/collect_cv184x_deps.sh`
- `scripts/build_cv184x.sh`
- `scripts/package_runtime.sh`
- `scripts/clean_tree.sh`

## 文档入口

- [架构说明](docs/ARCHITECTURE.md)
- [算法说明](docs/ALGORITHMS.md)
- [开发指南](docs/DEV_GUIDE.md)
- [MMF 与媒体图](docs/MMF.md)
- [绑定设计](docs/BINDINGS.md)
- [双核控制](docs/DUAL_CORE.md)
- [API 与 demo 对照](docs/ALG_INFO.md)

## 关于 `--firmware`

很多 BMRuntime 模型在板端运行时需要显式指定：

```sh
--firmware ./firmware/libbm1688_kernel_module.so
```

原因不是“每帧都要重新加载 firmware”，而是模型会话初始化时需要告诉 BMRuntime 使用哪套 TPU kernel 模块。这个路径通常在 `Detector` / `Classifier` 的 `load/open` 阶段通过环境变量传给 BMRuntime，一次初始化后，后续每帧只是复用同一个 runtime 句柄。

## 建议

- 相机流优先直接给 `VIDEO_FRAME_INFO_S`。
- 单张图保留图片路径接口，便于验证与离线测试。
- 多模型同时运行时，每个模型实例单独持有自己的 runtime，不要在业务层反复创建和销毁。
