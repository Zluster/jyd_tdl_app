# 算法说明

本文说明当前算法运行时布局、后处理职责以及扩展方式。

## 运行时分层

算法实现主要在：

- `src/algorithm/algorithm_engine.cpp`
- `src/algorithm/nn_yolov5.cpp`
- `src/algorithm/nn_yolov8.cpp`
- `src/algorithm/nn_classifier.cpp`
- `src/algorithm/nn_feature.cpp`
- `src/algorithm/nn_scrfd.cpp`
- `src/algorithm/nn_face_attribute.cpp`
- `src/algorithm/nn_plate_recognizer.cpp`

对外公开包装主要是：

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

包装层负责选择运行时，运行时负责真正的预处理、推理和后处理。

## 当前算法家族

- `NnYolov5`：YOLOv5 风格目标检测
- `NnYolov8`：YOLOv8 风格目标检测
- `NnClassifier`：分类
- `NnFeature`：特征提取
- `NnScrfd`：SCRFD 人脸检测
- `NnFaceAttribute`：人脸属性
- `NnPlateRecognizer`：车牌识别
- `KeypointDetector`：关键点
- `SemanticSegmenter`：语义分割
- `InstanceSegmenter`：实例分割
- `LaneDetector`：车道线
- `VoiceActivityDetector`：语音活动检测

## YOLOv5 当前实现

`YOLOv5` 当前负责：

- 模型打开
- 图片或原生帧输入统一
- letterbox 预处理
- `BGR -> RGB`
- 输入量化适配
- BMRuntime 推理
- 输出反量化
- anchor 解码
- 阈值过滤
- NMS
- 标签映射

### 当前输入支持

- 图片路径
- `Frame`
- `VIDEO_FRAME_INFO_S`

### 当前原生帧支持格式

- `RGB888`
- `BGR888`
- `RGB888_PLANAR`
- `BGR888_PLANAR`
- `NV12`
- `NV21`
- `YUV400`

### 输出

- `AlgorithmResult.boxes`
- `AlgorithmResult.labels`

## YOLOv8 当前实现

`YOLOv8` 负责：

- 图片 / 帧输入
- letterbox
- DFL 解码
- 阈值过滤
- NMS
- 标签映射

输出仍然是：

- `AlgorithmResult.boxes`
- `AlgorithmResult.labels`

## 分类与特征

`Classifier` 负责：

- resize / 通道转换 / 归一化
- score 向量解析
- softmax 可选
- top-k 输出

输出：

- `AlgorithmResult.classes`
- `AlgorithmResult.labels`

`FeatureExtractor` 负责：

- 图像预处理
- embedding 导出
- 可选 L2 归一化

输出：

- `AlgorithmResult.feature`

## 人脸和 OCR

`SCRFD` 负责：

- box 解码
- 5 点 landmark 解码
- NMS

输出：

- `AlgorithmResult.boxes`
- `AlgorithmResult.boxes[i].landmarks`

`FaceAttributeClassifier` 负责：

- 人脸 crop
- 多头输出映射
- 属性结果导出

输出：

- `AlgorithmResult.attributes`

`PlateRecognizer` 负责：

- ROI 裁剪
- CTC greedy decode
- 文本输出

输出：

- `AlgorithmResult.text`

## 扩展规则

新增算法 wrapper 时，建议遵守：

1. `load/open` 里只做一次性初始化。
2. `predict/predictFrame` 只做单次推理路径。
3. 预处理写在运行时类内部。
4. 后处理也写在运行时类内部，不要散落到 demo。
5. 最终统一写回稳定结果结构。

开发模板见：

- [开发指南](DEV_GUIDE.md)
