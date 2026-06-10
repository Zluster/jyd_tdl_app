# API 与 Demo 对照

## 公共 API 对照表

| API | 主要输出 | 对应 demo |
| --- | --- | --- |
| `Detector` | `AlgorithmResult.boxes / labels` | `tdl_detect_demo`、`tdl_yolov5_demo`、`tdl_yolov8_demo`、`tdl_camera_detect_demo`、`tdl_camera_rtsp_demo` |
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
| `Camera` | `Frame` | `tdl_camera_capture_demo` 以及所有 `tdl_camera_*` |
| `MultiStagePipeline` | 多阶段聚合结果 | `tdl_face_attr_pipeline_demo` |

## 高级媒体 API 对照表

| API | 作用 | 对应 demo |
| --- | --- | --- |
| `SensorMedia` | sensor / MIPI / ISP / VI / VPSS 启动 | 多个 `tdl_camera_*` demo |
| `Mmf` | `SYS/VB/VPSS/...` 快速启动 | 多个 `tdl_camera_*` demo |
| `RtspStreamer` | RTSP 推流 | `tdl_camera_rtsp_demo` |
| `OsdRegion` | OSD 区域 | `tdl_osd_demo`、`tdl_media_smoke_demo` |
| `GraphicVoLayer` | 图形显示层 | `tdl_graphic_vo_demo` |
| `VdecChannel` | 解码通道 | `tdl_vdec_demo` |
| `VencChannel` | 编码通道 | `tdl_media_smoke_demo` |

## 后处理怎么理解

先记一个统一框架：

1. 预处理
2. 推理
3. 输出反量化或解析
4. 后处理解码
5. 过滤 / 聚合
6. 写回稳定结果结构

### YOLOv5

- 先做 letterbox
- 再 `BGR -> RGB`
- 再除以 `255`
- 输出按网格和 anchor 解码
- 分数是 `obj * class_score`
- 最后回映射到原图并做 NMS

### YOLOv8

- 同样先 letterbox
- 再做 DFL 解码
- 计算分类分数
- 回映射到原图
- 做 NMS

### Classifier

- resize / 通道转换 / 归一化
- 读 score 向量
- 需要时 softmax
- 取 top-k

### FeatureExtractor

- 和分类预处理相近
- 输出不是类别，而是 embedding
- 需要时做 L2 归一化

### SCRFD

- 解码 face box
- 解码 5 点 landmark
- 阈值过滤
- NMS

### FaceAttribute

- 可先做人脸 ROI 裁剪
- 多头输出映射为属性
- 不需要 NMS

### PlateRecognizer

- 可先裁剪车牌 ROI
- 做 CTC greedy decode
- 去 blank、去重复
- 拼成最终字符串
