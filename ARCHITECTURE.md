# 架构说明

工程分成两条并行主线：

1. 媒体栈
2. 算法栈

二者只通过 `Frame / FrameSource / FrameSink` 交汇。这样相机、RTSP、图片输入、算法包装和输出链路不会互相污染。

## 媒体栈

公开类主要包括：

- `MediaSystem`
- `SysContext`
- `VideoBufferManager`
- `VideoBufferPool`
- `ViChannel`
- `VpssGroup`
- `MediaLink`
- `FrameReader`
- `Camera`
- `Mmf`
- `SensorMedia`
- `VdecChannel`
- `VencChannel`
- `VoOutput`
- `GraphicVoLayer`
- `OsdRegion`
- `RegionOverlay`
- `RtspStreamer`
- `MipiDevice`

职责划分：

- `MediaSystem / SysContext`：管理 `SYS/VB/ION`
- `ViChannel / VpssGroup / MediaLink`：搭建媒体图
- `FrameReader / Camera`：读取原生帧
- `VencChannel / VdecChannel / RtspStreamer`：编码、解码、推流
- `VoOutput / GraphicVoLayer / OsdRegion / RegionOverlay`：显示与叠图
- `SensorMedia / Mmf`：把板级启动过程封到更高一层

板级差异相关代码放在：

- `src/media/private/`

上层业务原则上不要直接碰这里。

## 算法栈

算法侧分四层：

1. 输入抽象：`FrameSource`
2. 模型运行时：`NnBase` 及其子类
3. 输出抽象：`FrameSink`
4. 编排层：`Pipeline / MultiStagePipeline`

当前主要运行时：

- `NnYolov5`
- `NnYolov8`
- `NnClassifier`
- `NnFeature`
- `NnScrfd`
- `NnFaceAttribute`
- `NnPlateRecognizer`

这些类各自负责：

- 模型加载
- 预处理
- 调用 BMRuntime 或 `tdl_sdk`
- 输出 tensor 解码
- 后处理
- 填充统一结果结构

## 统一结果结构

大多数上层接口最后都会收敛到以下结构：

- `AlgorithmResult`
- `KeypointResult`
- `SemanticSegmentationResult`
- `InstanceSegmentationResult`
- `LaneDetectionResult`
- `VoiceActivityResult`

这样做的目的是让上层业务代码不需要理解底层每个模型的原始 tensor 形状。

## 为什么这样设计

核心目标有三个：

1. 把厂商 SDK 细节压到内部。
2. 让图片输入和相机输入走同一套算法调用面。
3. 为多模型、多阶段和脚本绑定保留稳定接口。

## 当前推荐入口

单模型场景优先用：

- `Detector`
- `Classifier`
- `FaceDetector`
- `FeatureExtractor`

多阶段场景优先用：

- `MultiStagePipeline`

媒体控制或板级能力优先用：

- `advanced.hpp`
