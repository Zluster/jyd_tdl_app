# API 与 Demo 对照

本文给出当前公开 API 与 demo 程序的对应关系，方便快速定位示例代码。

## 公共算法 API 对照

| API | 主要输出 | 对应 demo |
| --- | --- | --- |
| `Detector` | `AlgorithmResult.boxes / labels` | `tdl_detect_demo`、`tdl_yolov5_demo`、`tdl_yolov8_demo`、`tdl_camera_detect_demo` |
| `Classifier` | `AlgorithmResult.classes / labels` | `tdl_classify_demo`、`tdl_camera_classify_demo` |
| `FaceDetector` | `AlgorithmResult.boxes[*].landmarks` | `tdl_face_detect_demo`、`tdl_face_attr_pipeline_demo` 第一阶段 |
| `FaceAttributeClassifier` | `AlgorithmResult.attributes` | `tdl_face_attribute_demo`、`tdl_face_attr_pipeline_demo` 第二阶段 |
| `FeatureExtractor` | `AlgorithmResult.feature` | `tdl_feature_demo`、`tdl_camera_feature_demo` |
| `PlateRecognizer` | `AlgorithmResult.text` | `tdl_plate_recognize_demo` |
| `KeypointDetector` | `KeypointResult.points` | `tdl_keypoint_demo` |
| `SemanticSegmenter` | `SemanticSegmentationResult` | `tdl_semantic_seg_demo` |
| `InstanceSegmenter` | `InstanceSegmentationResult` | `tdl_instance_seg_demo` |
| `LaneDetector` | `LaneDetectionResult` | `tdl_lane_demo` |
| `VoiceActivityDetector` | `VoiceActivityResult` | `tdl_vad_demo` |

## 媒体与相机 API 对照

| API | 作用 | 对应 demo |
| --- | --- | --- |
| `Camera` | 获取 `Frame` | `tdl_camera_capture_demo` 以及所有 `tdl_camera_*` |
| `SensorMedia` | sensor / MIPI / ISP / VI / VPSS 启动 | 多个 `tdl_camera_*` demo |
| `Mmf` | `SYS/VB/VPSS/...` 快速启动 | 多个 `tdl_camera_*` demo |
| `RtspStreamer` | RTSP 推流 | `tdl_camera_rtsp_demo` |
| `OsdRegion` | OSD 区域叠加 | `tdl_osd_demo`、`tdl_media_smoke_demo` |
| `GraphicVoLayer` | 图形显示层 | `tdl_graphic_vo_demo` |
| `VdecChannel` | 解码通道 | `tdl_vdec_demo` |
| `VencChannel` | 编码通道 | `tdl_media_smoke_demo` |

## 音频相关 demo

| demo | 作用 |
| --- | --- |
| `tdl_audio_aio_demo` | AI 录音、AO 播放、基础 AIO 验证 |

## 后处理如何理解

统一理解为六步：

1. 预处理
2. 推理
3. 输出解析或反量化
4. 后处理解码
5. 过滤或聚合
6. 写回稳定结果结构

### YOLOv5

- `letterbox`
- `BGR -> RGB`
- 输入归一化
- anchor 解码
- 分数计算
- NMS

### YOLOv8

- `letterbox`
- DFL 解码
- 分类分数解析
- NMS

### Classifier

- resize
- 通道转换
- 归一化
- score 向量解析
- top-k

### FeatureExtractor

- 图像预处理
- embedding 输出
- 按需 L2 归一化

### SCRFD

- box 解码
- landmark 解码
- NMS

### PlateRecognizer

- 车牌 ROI 处理
- CTC decode
- 文本拼接
