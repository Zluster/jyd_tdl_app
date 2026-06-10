# 算法测试与 `model_tool` 分析指南

本文面向当前 `tdl_app_sdk` 仓库，整理每个已接入算法的测试方法、对应 demo、典型命令、输出判读方式，以及如何借助 `model_tool` 分析模型输入输出并反推后处理逻辑。

## 1. 使用范围与前提

本文覆盖当前仓库中的算法类与对应 demo，重点包括：

- 检测：`Detector` / `FaceDetector`
- 分类：`Classifier`
- 特征：`FeatureExtractor`
- 人脸属性：`FaceAttributeClassifier`
- 车牌识别：`PlateRecognizer`
- 关键点：`KeypointDetector`
- 人脸稠密关键点：`tdl_face_dense_keypoint_demo`
- 实例分割：`InstanceSegmenter`
- 语义分割：`SemanticSegmenter`
- 单目标跟踪：`SingleObjectTracker`
- 车道线：`LaneDetector`
- 语音活动检测：`VoiceActivityDetector`

默认约定：

- 仓库根目录为 `/mnt/sd/tdl_app_sdk_cv184x` 或 `/home/jyd/zwz/sophpi/tdl_app_sdk`
- 已执行 `. ./env.sh`
- `firmware/libbm1688_kernel_module.so` 可用
- `configs/model_specs/*.mud` 与 `models/cv184x/*.bmodel` 路径匹配

如果你的本地分支还保留旧的 `.ini`，而 SDK 主机或板端已经切到 `.mud`，优先以板端当前实际存在的 `.mud` 为准。

## 2. 通用测试流程

推荐所有视觉算法都按下面顺序测试：

1. 先确认 `model_spec` 指向的 `.bmodel` 存在。
2. 用 `model_tool` 查看模型输入输出、shape、dtype、scale、zero point。
3. 再运行 demo，先看终端输出，再决定是否保存可视化结果图。
4. 如果结果异常，先判断是：
   - 模型本身输出不对
   - `model_spec` 配置不对
   - 后处理映射不对
   - 输入预处理不对

## 3. `model_tool` 怎么用

### 3.1 工具路径

当前 SDK 主机上的工具路径是：

```sh
/home/jyd/zwz/sophpi/tdl_app_sdk/configs/model_tool
```

它不是目录，而是一个 ELF 可执行文件。

### 3.2 常用命令

查看模型信息：

```sh
./configs/model_tool --info models/cv184x/xxx.bmodel
```

典型输出会包含：

- `input`: 输入名字、shape、dtype、scale、zero point
- `output`: 输出名字、shape、dtype、scale、zero point
- `device mem size`: TPU 显存开销
- `host mem size`: 主机侧内存开销

### 3.3 常见报错

如果在 SDK 主机上运行时报：

```sh
error while loading shared libraries: libomp.so.5
```

说明当前主机缺少 OpenMP 运行时。处理方式：

- 优先直接在板端运行 `model_tool`
- 或者在 SDK 主机补齐 `libomp.so.5`

### 3.4 怎么从 `model_tool` 输出判断后处理类型

#### 分类 / 特征

如果输出类似：

```text
output: xxx, [1, N]
```

通常表示：

- 分类：对 `N` 维分数做 `softmax` 或直接 `top-k`
- 特征：直接把 `N` 维向量导出，必要时做 `L2 normalize`

#### YOLOv5 / YOLOv8 检测

如果看到 3 个尺度输出，比如 `80x80`、`40x40`、`20x20`，通常是检测模型。

- YOLOv5：常见是 anchor-based 输出
- YOLOv8：常见是 DFL box 分支 + class 分支

#### SCRFD 人脸检测

如果输出是 3 组 stride 分支，并且每组有：

- score
- bbox
- kps

那么后处理通常是：

- 解 score
- 解 box
- 解 5 点 landmark
- 做 NMS

#### 关键点

如果输出直接是坐标或坐标+置信度，后处理通常是直接缩放回原图。

如果输出是检测头格式，则需要先解框，再解关键点。

#### 实例分割

如果输出里同时有：

- 检测框分支
- 类别分支
- mask coeff 分支
- proto 分支

那么通常是 YOLOv8/YOLO11 seg 类模型，后处理是：

1. 先解检测框
2. 再取对应 mask coeff
3. 与 proto 做线性组合
4. 上采样并裁切回原图

#### 语义分割

如果输出是：

- `ArgMax` 类整型图
- 或者 `C x H x W` 类别图

那么通常是逐像素分类。

#### 跟踪

如果模型有两个输入，例如：

- `template_image`
- `search_image`

并输出：

- `cls_Sigmoid`
- `bbox_Add`

那么通常是 Siamese 单目标跟踪结构，后处理是：

1. 初始化 template patch
2. 当前帧取 search patch
3. 从 score map 找峰值位置
4. 从 bbox map 回归目标框

#### VAD

如果输入不是图像而是 `pcm16le_mono`，并且输出与时间片段相关，就是语音活动检测，不属于视觉模型。

## 4. `model_spec` 关键字段怎么理解

示例：

```ini
[basic]
type = bmodel
model = ../../models/cv184x/pose_yolov8n_pose_cv184x_int8_sym.bmodel

[extra]
runtime = keypoint
task = keypoint
model_type = KEYPOINT_YOLOV8POSE
input_type = rgb
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
```

字段说明：

- `basic.model`: 模型路径
- `extra.runtime`: 运行时类型，决定走哪个算法实现
- `extra.task`: 任务语义，供上层调度参考
- `extra.model_type`: 更具体的模型类别
- `extra.input_type`: `rgb` 或 `bgr`
- `extra.mean` / `extra.scale`: 预处理归一化
- `extra.labels`: 分类或检测标签
- `extra.apply_softmax`: 分类模型是否在后处理中补 softmax
- `extra.normalize`: 特征模型是否做 `l2`

## 5. 算法与 demo 总表

| 算法 | 主类 / 入口 | 主测试 demo | 结果形式 |
| --- | --- | --- | --- |
| 通用检测 | `Detector` | `tdl_detect_demo` | `boxes + labels` |
| YOLOv5 检测 | `Detector("YOLOV5")` | `tdl_yolov5_demo` / `tdl_detect_demo` | `boxes + labels` |
| YOLOv8 检测 | `Detector("YOLOV8")` | `tdl_yolov8_demo` / `tdl_detect_demo` | `boxes + labels` |
| 人脸检测 | `FaceDetector` | `tdl_face_detect_demo` | `face boxes + landmarks` |
| 分类 | `Classifier` | `tdl_classify_demo` | `classes + labels` |
| 特征提取 | `FeatureExtractor` | `tdl_feature_demo` | `feature vector` |
| 人脸属性 | `FaceAttributeClassifier` | `tdl_face_attribute_demo` | `attributes` |
| 车牌识别 | `PlateRecognizer` | `tdl_plate_recognize_demo` | `text` |
| 关键点 | `KeypointDetector` | `tdl_keypoint_demo` | `points` |
| 稠密人脸关键点 | 独立 demo | `tdl_face_dense_keypoint_demo` | `478 dense points` |
| 实例分割 | `InstanceSegmenter` | `tdl_instance_seg_demo` | `instances + mask` |
| 语义分割 | `SemanticSegmenter` | `tdl_semantic_seg_demo` | `class map` |
| 单目标跟踪 | `SingleObjectTracker` | `tdl_single_object_tracker_demo` | `tracked box` |
| 车道线 | `LaneDetector` | `tdl_lane_demo` | `lanes` |
| VAD | `VoiceActivityDetector` | `tdl_vad_demo` | `speech segments` |

说明：

- `tdl_detect_demo` 是当前最通用的检测入口，推荐优先使用。
- `tdl_yolov5_demo` 和 `tdl_yolov8_demo` 更像快捷包装。
- `tdl_face_dense_keypoint_demo.cpp` 当前存在源码，但不一定在默认 CMake 目标中启用，取决于你的分支。

## 6. 各算法测试命令与判读

下面命令默认在板端仓库根目录执行：

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh
```

### 6.1 通用目标检测

推荐 spec：

- `configs/model_specs/yolov5s_det_coco80.mud`
- `configs/model_specs/yolov8n_det_coco80.mud`
- `configs/model_specs/yolov8n_det_roads.mud`

命令：

```sh
./bin/tdl_detect_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/yolov8n_det_coco80.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --output /mnt/sd/out_detect.jpg
```

输出重点：

- `boxes`: 检测框数量
- `id` / `score`: 类别与置信度
- `box=(x1,y1,x2,y2)`: 原图坐标

后处理重点：

- YOLOv5：anchor decode + NMS
- YOLOv8：DFL decode + NMS

### 6.2 人脸检测

推荐 spec：

- `configs/model_specs/scrfd_real.mud`

命令：

```sh
./bin/tdl_face_detect_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/scrfd_real.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --output /mnt/sd/out_face.jpg
```

输出重点：

- `boxes`
- `landmarks=5` 或 `landmarks=0`

判读：

- 如果 box 正常但 `landmarks=0`，通常是后处理没有匹配到 kps 输出，或者该路径当前没有解 5 点。

### 6.3 分类

推荐 spec：

- `configs/model_specs/cls_hand_gesture.mud`
- `configs/model_specs/plant_classifier.mud`

命令：

```sh
./bin/tdl_classify_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/plant_classifier.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --top-k 5 \
  --output /mnt/sd/out_cls.jpg
```

输出重点：

- `classes`
- `[i] id= score= label=`

后处理重点：

- 是否需要 `softmax`
- 标签顺序是否与训练导出一致
- 输入 `mean/scale` 是否和量化前预处理一致

### 6.4 特征提取

推荐 spec：

- `configs/model_specs/feature_cviface.mud`

命令：

```sh
./bin/tdl_feature_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/feature_cviface.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

输出重点：

- `feature elements`

判读：

- 主要看向量长度是否正确
- 如果后续做检索，要再验证是否已做 `l2 normalize`

### 6.5 人脸属性

推荐 spec：

- 使用当前实际的人脸属性 `.mud`
- 如果仓库里暂无现成 spec，可从 `template_face_attribute.mud/.ini` 派生

命令：

```sh
./bin/tdl_face_attribute_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --roi 40,30,120,140 \
  --output /mnt/sd/out_face_attr.jpg
```

输出重点：

- `attributes`

判读：

- 如果属性值整体固定不变，优先检查输出映射、softmax/sigmoid、ROI 裁切是否正确。

### 6.6 车牌识别

推荐 spec：

- 使用当前实际车牌识别 `.mud`
- 如果仓库里暂无现成 spec，可从 `template_plate_recognizer.mud/.ini` 派生

命令：

```sh
./bin/tdl_plate_recognize_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_plate_recognizer.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --roi 100,120,180,60 \
  --output /mnt/sd/out_plate.jpg
```

输出重点：

- `text`

后处理重点：

- 当前实现是 CTC greedy decode
- 重点检查字符表顺序、blank 去重逻辑、输入宽高是否匹配

### 6.7 一般关键点 / Pose

推荐 spec：

- `configs/model_specs/pose_yolov8.mud`

命令：

```sh
./bin/tdl_keypoint_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/pose_yolov8.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/out_keypoint.jpg
```

输出重点：

- `points`
- 每个点的 `x y score`

判读：

- 如果点整体偏移，优先检查：
  - 输入 resize / letterbox 映射
  - 输出坐标是否正确映射回原图
  - 点数是否由输出通道自动推断正确

### 6.8 稠密人脸关键点

推荐 spec：

- 检测器：`configs/model_specs/scrfd_real.mud` 或 `yolov8_face_real.mud`
- 稠密关键点：`configs/model_specs/face_dense_real.mud`

命令：

```sh
./run_face_dense_keypoint_demo.sh \
  --image /mnt/sd/test.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --keypoint-model-spec ./configs/model_specs/face_dense_real.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --roi-expand-ratio 0.2 \
  --output /mnt/sd/out_face_dense.jpg
```

输出重点：

- `face_count`
- `dense_point_count`
- 调试信息中的 `roi=(affine_aligned,256x256)` 或其他 crop 模式

判读：

- 如果下巴、轮廓偏移，优先怀疑：
  - 5 点对齐矩阵
  - box-based fallback 裁切策略
  - 关键点输出坐标尺度映射

### 6.9 实例分割

推荐 spec：

- `configs/model_specs/segTest_yolov8n_seg.mud`
- `configs/model_specs/segTest_yolo11n_seg.mud`

命令：

```sh
./bin/tdl_instance_seg_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/segTest_yolov8n_seg.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/out_seg.jpg
```

调试命令：

```sh
TDL_APP_SEG_DEBUG=1 ./bin/tdl_instance_seg_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/segTest_yolov8n_seg.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/out_seg.jpg
```

输出重点：

- `instances`
- 每个实例的 `class_id score box outline_points`

后处理重点：

- 检测框 decode
- coeff/proto 对齐
- mask 上采样后裁切回原图

### 6.10 语义分割

推荐 spec：

- `configs/model_specs/your_topformer_seg.mud`
- 模板：`configs/model_specs/template_semantic_segmentation.mud`

命令：

```sh
./bin/tdl_semantic_seg_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_topformer_seg.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

输出重点：

- `output_width`
- `output_height`
- `pixels`

判读：

- 当前 demo 默认不生成彩色 mask 图
- 如果模型输出是 `ArgMax [1,H,W]`，当前最稳的检查方式是先确认像素数与输出尺寸匹配

### 6.11 单目标跟踪

推荐 spec：

- `configs/model_specs/your_feartrack.mud`

命令：

```sh
./bin/tdl_single_object_tracker_demo \
  --template-image /mnt/sd/template.jpg \
  --search-image /mnt/sd/search.jpg \
  --model-spec ./configs/model_specs/your_feartrack.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --init-box 100,80,180,220 \
  --output /mnt/sd/out_track.jpg
```

调试命令：

```sh
TDL_APP_TRACK_DEBUG=1 ./bin/tdl_single_object_tracker_demo \
  --template-image /mnt/sd/template.jpg \
  --search-image /mnt/sd/search.jpg \
  --model-spec ./configs/model_specs/your_feartrack.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --init-box 100,80,180,220 \
  --output /mnt/sd/out_track.jpg
```

输出重点：

- `score`
- `box`

后处理重点：

- template patch 裁切
- search patch 裁切
- score map 峰值位置
- bbox map 回归框

### 6.12 车道线

推荐 spec：

- `configs/model_specs/your_lane_detection.mud`
- 模板：`configs/model_specs/template_lane_detection.mud`

命令：

```sh
./bin/tdl_lane_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_lane_detection.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

输出重点：

- `lanes`
- `lane_state`
- 每条线的 `start / end / score`

说明：

- 当前 `LSTR_DET_LANE` 已切到自研 BMRT runtime
- 默认按 ImageNet 归一化处理；如果你的导出模型预处理不同，需要在 `.mud` 里显式改 `mean/scale`
- 当前结果结构仍是简化版，只输出每条 lane 的 `start/end/score`

### 6.13 语音活动检测

推荐 spec：

- `configs/model_specs/your_vad_fsmn.mud`
- 模板：`configs/model_specs/template_vad_fsmn.mud`

命令：

```sh
./bin/tdl_vad_demo \
  --pcm /mnt/sd/test_16k_mono.pcm \
  --model-spec ./configs/model_specs/your_vad_fsmn.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --chunk-bytes 3200
```

输出重点：

- `has_speech`
- `start_event`
- `end_event`
- `segments`

判读：

- `chunk-bytes` 用于流式测试
- 如果整段跑正常、分块跑异常，优先检查状态缓存逻辑而不是模型本身

## 7. 多阶段 demo

### 7.1 人脸检测 + 属性流水线

命令：

```sh
./bin/tdl_face_attr_pipeline_demo \
  --image /mnt/sd/test.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --attribute-model-spec ./configs/model_specs/your_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --output /mnt/sd/out_face_attr_pipeline.jpg
```

用途：

- 验证多阶段 pipeline 是否正确传递 ROI
- 验证检测结果是否正确喂给属性分类器

## 8. 当前建议的主测 demo

如果只是做算法回归，建议优先使用下面这些主入口：

- 检测：`tdl_detect_demo`
- 人脸：`tdl_face_detect_demo`
- 分类：`tdl_classify_demo`
- 特征：`tdl_feature_demo`
- 人脸属性：`tdl_face_attribute_demo`
- 关键点：`tdl_keypoint_demo`
- 稠密人脸关键点：`tdl_face_dense_keypoint_demo`
- 实例分割：`tdl_instance_seg_demo`
- 语义分割：`tdl_semantic_seg_demo`
- 跟踪：`tdl_single_object_tracker_demo`
- 车道线：`tdl_lane_demo`
- VAD：`tdl_vad_demo`

## 9. 当前实现状态补充

截至当前分支：

- `Detector` / `FaceDetector` / `Classifier` / `FeatureExtractor` 已走自研 BMRT runtime
- `FaceAttributeClassifier` / `PlateRecognizer` 底层也已走自研 BMRT runtime
- `KeypointDetector` / `InstanceSegmenter` / `SemanticSegmenter` 是“自研 runtime + legacy fallback”并存
- `SingleObjectTracker` 已新增自研 BMRT 原型
- `LaneDetector` / `VoiceActivityDetector` 仍主要走 vendor 路径

这意味着测试时如果发现异常，需要先区分当前算法到底走的是：

- 自研 BMRT runtime
- 还是 vendor legacy runtime

最简单的方法是：

- 看终端里是否打印了我们新增的 debug 标记
- 看二进制 `strings` 是否包含对应调试字符串
- 对照当前 `model_type` 是否已经在自研路径覆盖范围内
