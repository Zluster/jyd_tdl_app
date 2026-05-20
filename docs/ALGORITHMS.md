# 算法说明

本文说明当前算法运行时布局、主要后处理职责以及新增算法时建议遵循的模式。

## 当前算法目录

算法核心实现主要位于：

- `src/algorithm/algorithm_engine.cpp`
- `src/algorithm/nn_yolov5.cpp`
- `src/algorithm/nn_yolov8.cpp`
- `src/algorithm/nn_classifier.cpp`
- `src/algorithm/nn_feature.cpp`
- `src/algorithm/nn_scrfd.cpp`
- `src/algorithm/nn_face_attribute.cpp`
- `src/algorithm/nn_plate_recognizer.cpp`

## 对外封装类

当前常用算法封装包括：

- `Detector`
- `Classifier`
- `FaceDetector`
- `FaceAttributeClassifier`
- `PlateRecognizer`
- `FeatureExtractor`
- `KeypointDetector`
- `SemanticSegmenter`
- `InstanceSegmenter`
- `LaneDetector`
- `VoiceActivityDetector`

这些封装类负责：

- 选择运行时实现
- 提供统一的加载、推理入口
- 把结果写回统一结构

## 当前算法家族

### 检测

- `NnYolov5`
- `NnYolov8`
- `NnScrfd`

### 分类与特征

- `NnClassifier`
- `NnFeature`

### 人脸与 OCR

- `NnFaceAttribute`
- `NnPlateRecognizer`

### 其它

- `KeypointDetector`
- `SemanticSegmenter`
- `InstanceSegmenter`
- `LaneDetector`
- `VoiceActivityDetector`

## YOLOv5 当前实现

### 输入支持

- 图片路径
- `Frame`
- `VIDEO_FRAME_INFO_S`

### 当前支持的原生帧格式

- `PIXEL_FORMAT_RGB_888`
- `PIXEL_FORMAT_BGR_888`
- `PIXEL_FORMAT_RGB_888_PLANAR`
- `PIXEL_FORMAT_BGR_888_PLANAR`
- `PIXEL_FORMAT_NV12`
- `PIXEL_FORMAT_NV21`
- `PIXEL_FORMAT_YUV_400`

### 主要处理流程

1. 统一得到输入图像
2. `letterbox`
3. `BGR -> RGB`
4. 输入量化适配
5. BMRuntime 推理
6. 输出反量化
7. anchor 解码
8. 阈值过滤
9. NMS
10. 写回 `AlgorithmResult.boxes` 和 `labels`

## YOLOv8 当前实现

主要处理流程与 YOLOv5 类似，但后处理重点是：

- DFL 解码
- 分类分数解析
- NMS

最终输出仍然统一写入：

- `AlgorithmResult.boxes`
- `AlgorithmResult.labels`

## 分类与特征

### Classifier

负责：

- resize
- 通道转换
- 归一化
- score 向量解析
- softmax（按需）
- top-k 输出

最终输出：

- `AlgorithmResult.classes`
- `AlgorithmResult.labels`

### FeatureExtractor

负责：

- 图像预处理
- embedding 导出
- 可选 L2 归一化

最终输出：

- `AlgorithmResult.feature`

## 人脸与车牌

### SCRFD

负责：

- 人脸框解码
- 5 点 landmark 解码
- 阈值过滤
- NMS

最终输出：

- `AlgorithmResult.boxes`
- `AlgorithmResult.boxes[i].landmarks`

### FaceAttributeClassifier

负责：

- 人脸 ROI 裁剪
- 多头输出映射
- 属性结果导出

最终输出：

- `AlgorithmResult.attributes`

### PlateRecognizer

负责：

- 车牌 ROI 处理
- CTC greedy decode
- 去 blank、去重复
- 输出最终文本

最终输出：

- `AlgorithmResult.text`

## 新增算法时的建议规则

### 1. 初始化只做一次

`load/open` 阶段只做一次性初始化，例如：

- 申请 device
- 设置 firmware 环境
- 创建 runtime
- 加载 bmodel

### 2. 单次推理路径要收敛

`predict/predictFrame` 只负责：

- 调预处理
- 调推理
- 调后处理
- 写回结果

### 3. 后处理必须放在运行时内部

不要把以下逻辑散落到 demo：

- NMS
- softmax
- anchor 解码
- DFL 解码
- CTC decode
- landmark 映射

### 4. 优先复用统一结果结构

如无必要，不要再新造很多零散结构，优先复用：

- `AlgorithmResult`
- `KeypointResult`
- `SemanticSegmentationResult`
- `InstanceSegmentationResult`
- `LaneDetectionResult`
