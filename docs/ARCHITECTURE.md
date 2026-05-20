# 架构说明

## 总体目标

`tdl_app_sdk` 的架构目标不是简单把厂商 SDK 再包一层，而是把“上层业务最常见的调用路径”整理成一组稳定接口，同时把板级差异、媒体图细节、模型加载细节尽量收口在内部实现里。

## 分层结构

整体可以分成四层：

### 1. 上层调用层

这一层是业务代码直接使用的接口，典型类包括：

- `Detector`
- `Classifier`
- `FaceDetector`
- `FaceAttributeClassifier`
- `FeatureExtractor`
- `PlateRecognizer`
- `Camera`
- `Pipeline`
- `MultiStagePipeline`

这层的职责是：

- 提供尽量稳定、易用的 API
- 暴露统一的 `ModelSessionConfig`、`InferOptions`、`AlgorithmResult`
- 尽量不让业务层直接碰 `bmrt`、`bmlib`、`CVI_MPI`

### 2. 算法运行时层

对应目录：

- `src/algorithm/`

主要职责：

- 预处理
- 推理调用
- 输出解析
- 后处理
- 结果填充

典型实现包括：

- `NnYolov5`
- `NnYolov8`
- `NnClassifier`
- `NnFeature`
- `NnScrfd`
- `NnFaceAttribute`
- `NnPlateRecognizer`

### 3. 媒体封装层

对应目录：

- `src/media/`
- `src/media/private/`

主要职责：

- `SYS/VB/VI/VPSS/VENC/VDEC/VO/RGN`
- sensor / MIPI / ISP 启动
- OSD、RTSP、显示、编码、抓图
- `Frame`、`FrameSource`、`FrameSink` 的统一抽象

### 4. 平台依赖层

主要来源：

- `third_party/cv184x/lib`
- 系统 `OpenCV`、`zlib`、`tinyalsa`

当前策略是：

- 平台专用依赖继续走仓库内 vendor 库
- 通用库尽量走系统库

## 数据流

### 单张图算法路径

```text
图片路径
  -> OpenCV 读图
  -> 预处理
  -> BMRuntime 推理
  -> 后处理
  -> AlgorithmResult
```

### 相机算法路径

```text
sensor / MIPI / ISP / VI / VPSS
  -> VIDEO_FRAME_INFO_S
  -> Frame / 原生帧输入
  -> 算法运行时
  -> AlgorithmResult
```

### 音视频媒体路径

```text
输入源
  -> VI / VPSS / VDEC / AI
  -> 中间处理
  -> VO / RTSP / VENC / AO
```

## 为什么强调统一结果结构

上层最怕的是每个算法都返回一套完全不同的结果结构。当前设计尽量把常见能力收敛到统一结构里：

- 检测结果：`AlgorithmResult.boxes`
- 分类结果：`AlgorithmResult.classes`
- 文本结果：`AlgorithmResult.text`
- 特征结果：`AlgorithmResult.feature`
- 属性结果：`AlgorithmResult.attributes`

这样做的好处是：

- demo 更简单
- 绑定层更容易
- 多阶段 pipeline 更容易拼接

## 当前已验证的重要结论

- 相机抓图当前可工作路径是：
  - `sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
  - `--backend vpss`
- 板端原生编译已可行
- 通用库已经切换为系统库
- `linux/fb.h` 缺失可通过兼容头处理
