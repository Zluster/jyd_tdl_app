# nn 推理结果手册

`nn.load(spec)` 根据 `.mud` 的 `[extra]` 自动识别模型族，返回统一的
`Model`。所有模型都通过 `Model.run(frame)` 推理；不同族的结果字段不同。

本文覆盖现有 10 个模型族，包括新增的人脸识别、手势识别、人体姿态分类和
自学习分类。

## 接口创建方式

SDK 中的统一模型工厂是 **`nn.load(...)`**，当前没有 `nn.model(...)` 这个
公开接口。因此请使用 `model = nn.load("xxx.mud")`，再调用
`model.run(frame)`；不要写成 `nn.model(...)`。

FearTrack 单目标跟踪也使用 `nn.load()`；多目标跟踪和计数是检测结果的跨帧
后处理，因此仍是 `nn` 下单独创建的对象。下表是新增能力的最短入口。

| 能力 | 创建方式 | 每帧调用 | 说明 |
|---|---|---|---|
| 自学习分类 | `nn.load("feature_xxx.mud", top_k=3)` | `model.run(frame)` | feature `.mud` 自动识别为 `self_learning` |
| 单目标视觉跟踪 | `nn.load("feartrack.mud")` | `tracker.run(frame)` | 先 `initialize()` 框选一个目标 |
| 多目标跟踪 | `nn.ObjectTracker(...)` | `tracker.update(det.boxes)` | `det` 是检测模型的 `run()` 结果 |
| 过线计数 | `nn.LineCounter(line_x=360)` | `counter.update(tracks)` | `tracks` 是跟踪器输出 |

`ObjectTracker` 的定位是检测结果的 ByteTrack 多目标跟踪/计数；它不加载
FearTrack 模型。`TargetTracker` 是复用检测框与 `ObjectTracker` 的轻量选择器。

`dara/nn.py` 是这些上层接口的唯一入口；应用不应直接创建
`tdl_py.SelfLearningClassifier` 或底层 ByteTrack。检查命令：

```sh
python3 -c 'from dara import nn; print(nn.load("feartrack.mud").family, hasattr(nn, "ObjectTracker"), hasattr(nn, "LineCounter"))'
```

## 通用调用规则

```python
from dara import camera, nn

model = nn.load("yolov8n_det_coco80.mud", threshold=0.5)
with camera.read() as frame:      # 默认 AI 通道：640x640、zero-copy
    result = model.run(frame)     # 必须在 with 块内完成推理

# 默认结果是纯 Python 副本；离开 with 后仍可绘制和读取。
```

- `camera.read()` 返回的 `Frame` 引用 VPSS 缓冲；离开 `with` 后失效。
- 默认 `to_screen=True`：坐标已转为屏幕 `720x480` 坐标，可直接绘制 LVGL/OSD。
- `to_screen=False`：返回底层 `tdl_py` 原始结果，坐标为推理帧坐标（通常是
  640x640）。
- 无检测/无识别结果返回空列表，不返回 `None`；底层推理失败抛 `RuntimeError`。
- 短模型名会在 `/root/tdl_app_sdk_cv184x/configs/model_specs/` 中查找。

AI 通道的显示映射遵循同一 sensor 的 letterbox 规则。对 640x640 AI 帧：

```python
x_screen = x_frame + 40
y_screen = y_frame - 80
```

普通应用不需要自行转换；使用默认 `to_screen=True` 即可。

## 模型族总览

| family | 典型 spec | `run(frame)` 返回 | 主要结果字段 |
|---|---|---|---|
| `detection` | `yolov8n_det_coco80.mud` | `Result` | `boxes`、`label_of()` |
| `classification` | `cls_xxx.mud` | `Result` | `classes`、`label_of()` |
| `keypoint` | `yolov8n_pose_person17.mud` | `KeypointResult` | `points` |
| `seg` | `yolov8n_seg_coco80.mud` | `SegResult` | `instances` |
| `ocr` | `pp_ocr_xxx.mud` | `Result` | `boxes`、`attributes`、`text` |
| `face_dense` | `face_dense_real.mud` | `list[(Box, list[Point])]` | 脸框、稠密点 |
| `face_recognition` | `face_recognizer.mud` | `FaceRecognitionResult` | `faces` |
| `hand_gesture` | `hand_gesture.mud` | `HandGestureResult` | `hands` |
| `pose_classifier` | `pose_classifier.mud` | `PoseResult` | `points`、`label` |
| `self_learning` | 特征模型 spec | `SelfLearningResult` | `classes` |

`model.family` 可用于按模型族选择绘制逻辑。

## 基础数据结构

### Point

| 字段 | 类型 | 含义 |
|---|---|---|
| `x` / `y` | int | 默认屏幕坐标 |
| `score` | float | 关键点置信度，范围 0~1 |

### Box

| 字段 | 类型 | 含义 |
|---|---|---|
| `x1` `y1` `x2` `y2` | int | 左上与右下屏幕坐标 |
| `score` | float | 检测置信度 |
| `class_id` | int | 类别编号 |
| `landmarks` | `list[Point]` | SCRFD 人脸框附带的 5 个五官点；普通检测为空 |
| `width` / `height` | int | 框宽、高，只读属性 |

## 基础模型族

### detection：目标检测

`run()` 返回 `Result`，使用 `result.boxes`，标签通过
`result.label_of(box.class_id)` 获取。

```python
model = nn.load("yolov8n_det_coco80.mud", threshold=0.25)
with camera.read() as frame:
    result = model.run(frame)

for box in result.boxes:
    print(result.label_of(box.class_id), box.score, box.x1, box.y1)
```

### classification：图像分类

`run()` 返回 `Result`。`result.classes` 按分数降序排列，最多返回 `top_k`
项；每项有 `class_id` 与 `score`。本族不使用 `threshold`。

```python
model = nn.load("cls_xxx.mud", top_k=5)
with camera.read() as frame:
    result = model.run(frame)
for item in result.classes:
    print(result.label_of(item.class_id), item.score)
```

### keypoint：姿态或关键点

`run()` 返回 `KeypointResult`，字段为 `points`、`width`、`height`。人体姿态
模型是 COCO-17 点：0 鼻，1/2 左右眼，3/4 左右耳，5/6 左右肩，7/8 左右肘，
9/10 左右腕，11/12 左右髋，13/14 左右膝，15/16 左右踝。

```python
model = nn.load("yolov8n_pose_person17.mud")
with camera.read() as frame:
    result = model.run(frame)
for point in result.points:
    if point.score > 0.3:
        draw_cross(point.x, point.y)
```

建议画点阈值使用 `0.3`，骨架连线要求两端点均有足够分数。

### seg：实例分割

`run()` 返回 `SegResult`。每个 `result.instances` 元素包含 `inst.box` 和
`inst.outline`；`outline` 是未闭合的多边形点列表，需要由应用自行首尾连线。

### ocr：OCR / 车牌识别

`run()` 返回 `Result`。

- 有检测框时：`boxes[i]` 对应 `attributes[i]`，其 `name` 为
  `"ocr_text:<文本>"`。
- 整图识别时：`boxes` 为空，文本在 `result.text`。

### face_dense：人脸稠密关键点

内部先使用 SCRFD 人脸检测，再逐脸做稠密关键点。`run()` 返回
`list[(box, points)]`，当前最多处理两张脸。

```python
model = nn.load("face_dense_real.mud", threshold=0.5, expand=0.2)
with camera.read() as frame:
    faces = model.run(frame)
for box, points in faces:
    print("face", box.score, "points", len(points))
```

`dense=False` 时跳过第二级稠密模型，只返回 SCRFD 的 5 个五官点。

## 新增算法接口

### face_recognition：人脸识别与人脸库

使用 `face_recognizer.mud`，内部串联人脸检测与特征提取。`run()` 返回
`FaceRecognitionResult`，其中 `faces` 是 `Face` 列表。

| `Face` 字段 | 含义 |
|---|---|
| `box` | 人脸框，屏幕坐标 |
| `name` | 匹配到的人脸名称；未匹配时通常为 `unknown` |
| `score` | 与最佳样本的相似度 |
| `matched` | 是否达到 `match_threshold` |
| `class_id` | 底层类别编号 |
| `points` | 人脸附带的关键点列表 |

```python
face = nn.load("face_recognizer.mud", threshold=0.35,
               match_threshold=0.60, max_faces=3)

with camera.read() as frame:
    face.enroll(frame, "alice")        # 录入当前最大人脸，必须在 with 内
    result = face.run(frame)

for item in result.faces:
    print(item.name, item.score, item.matched, item.box.x1, item.box.y1)
```

人脸库操作：

```python
face.save_faces("/root/faces.bin")
face.load_faces("/root/faces.bin")
print(face.names())
face.clear()
```

`enroll()`、`names()`、`save_faces()`、`load_faces()` 仅适用于
`face_recognition`；`clear()` 会清空当前进程的人脸库，需自行 `save_faces()`
持久化。

### hand_gesture：手检测、21 点与手势分类

使用 `hand_gesture.mud`。内部执行手检测、单手 21 点关键点和基于关键点的
手势分类。`run()` 返回 `HandGestureResult`，其中 `hands` 是 `Hand` 列表。

| `Hand` 字段 | 含义 |
|---|---|
| `box` | 手框，屏幕坐标 |
| `keypoints` | 21 个手关键点，屏幕坐标 |
| `label` | 分类器原始标签 |
| `gesture` | 归一化手势名称 |
| `score` | 分类置信度 |

```python
hand = nn.load("hand_gesture.mud", threshold=0.35, max_hands=2)
with camera.read() as frame:
    result = hand.run(frame)

for item in result.hands:
    print(item.gesture, item.score)
    for point in item.keypoints:
        if point.score > 0.3:
            draw_dot(point.x, point.y)
```

当前分类器有 9 个原始输出标签：`fist`、`five`、`four`、`none`、`ok`、`one`、
`three`、`three2`、`two`。`three2` 归一化为 `three`；`none` 没有确定手势，
`gesture` 会是 `unknown`。因此应用应优先判断 `item.gesture`，并结合 `score`
过滤不稳定结果。

### pose_classifier：人体姿态分类

使用 `pose_classifier.mud`，流程为 COCO-17 人体关键点、关键点 EMA 平滑、
几何规则分类和时间窗口平滑。`run()` 返回 `PoseResult`。

| `PoseResult` 字段 | 含义 |
|---|---|
| `points` | COCO-17 关键点，屏幕坐标 |
| `label` | 平滑后的姿态类别 |
| `raw_label` | 当前帧规则分类结果，未做时间平滑 |
| `confidence` | 分类置信度 |
| `history_size` | 当前平滑窗口的有效帧数 |
| `keypoint_ms` | 本帧关键点推理耗时，毫秒 |
| `total_ms` | 整体姿态分类耗时，毫秒 |

```python
pose = nn.load("pose_classifier.mud", threshold=0.35,
               ema_alpha=0.65, smooth_frames=5)
with camera.read() as frame:
    result = pose.run(frame)

print(result.label, result.confidence)
for point in result.points:
    if point.score > 0.3:
        draw_cross(point.x, point.y)
```

有效姿态类别为：`standing`、`sitting`、`lying`、`left_hand_up`、
`right_hand_up`、`both_hands_up`。关键点不足或规则无法确定时，`label` 为
`unknown`。这不是通用时序动作识别模型，暂不识别跑步、挥手、下蹲等复杂动作。

### self_learning：特征样本库与相似度分类

使用 feature 模型的 `.mud`。先向类别加入多张图片样本，再以余弦相似度做
类别排序。`run()` 返回 `SelfLearningResult`。

| 字段 | 含义 |
|---|---|
| `classes` | `LearningClass` 列表，按相似度降序 |
| `feature_dim` | 特征向量维度 |
| `LearningClass.label` | 样本类别名称 |
| `LearningClass.score` | 与该类别样本库的相似度 |
| `LearningClass.sample_count` | 此类别已有样本数 |

```python
classifier = nn.load("feature_mobilenetv2_050_embedding_160.mud", top_k=3)

classifier.add_sample("cup", "/root/samples/cup_01.jpg")
classifier.add_sample("cup", "/root/samples/cup_02.jpg")
classifier.add_sample("bottle", "/root/samples/bottle_01.jpg")
classifier.save_bank("/root/object_classes.bank")

with camera.read() as frame:
    result = classifier.run(frame)
for item in result.classes:
    print(item.label, item.score, item.sample_count)
```

样本库操作：

```python
classifier.load_bank("/root/object_classes.bank")
print(classifier.class_count, classifier.sample_count)
classifier.clear()
```

除图片路径外，也支持在有效帧内直接录样本：

```python
with camera.read() as frame:
    classifier.add_frame("cup", frame)
```

相似度不是概率，必须使用多个类别、每类多张不同角度和光照的样本，并按实际场景
自行设置接受阈值。

> 推荐使用 SDK 随附的 `feature_mobilenetv2_050_embedding_160.mud`。它是
> CV184X 可加载的通用 1280 维特征模型；不要用人脸特征模型做物体自学习分类。

## `nn.load()` 的新增参数

| 参数 | 默认值 | 适用模型族 | 含义 |
|---|---:|---|---|
| `threshold` | 0.5 | detection、ocr、face_dense、face_recognition、hand_gesture、pose_classifier | 检测或关键点置信度阈值 |
| `top_k` | 5 | classification、self_learning | 最多返回条数 |
| `match_threshold` | 0.6 | face_recognition | 人脸相似度达到此值才标记 `matched=True` |
| `max_faces` | 3 | face_recognition | 每帧最多处理人脸数 |
| `max_hands` | 2 | hand_gesture | 每帧最多处理手数 |
| `ema_alpha` | 0.65 | pose_classifier | 当前点相对上一帧点的平滑权重 |
| `smooth_frames` | 5 | pose_classifier | 姿态标签时间平滑窗口长度 |
| `firmware` | `""` | 全部 | NPU 固件路径，通常不需要传入 |
| `to_screen` | `True` | 全部 | 是否转换为 720x480 屏幕坐标 |

`face_dense` 额外支持 `face_spec`、`expand`、`dense`；其余族会忽略不相关参数。

## 多目标跟踪与计数

### ObjectTracker：ByteTrack 多目标跟踪

`nn.ObjectTracker` 复用检测框完成卡尔曼预测、两阶段高低分匹配和匈牙利
分配。`update()` 直接接收 `result.boxes`，返回稳定的 `TrackedObject` 列表。

```python
detector = nn.load("yolov8n_det_coco80.mud", threshold=0.25)
tracker = nn.ObjectTracker(high_score=0.45, low_score=0.15,
                           iou_threshold=0.30, max_missed=30)
with camera.read() as frame:
    result = detector.run(frame)
tracks = tracker.update(result.boxes)
for track in tracks:
    print(track.track_id, track.box, track.age)
```

`TrackedObject` 字段：`track_id`、`box`、`age`、`missed`、
`previous_center_x`、`center_x`、`center_y`。默认不返回本帧未匹配的轨迹；需要
预测轨迹时使用 `tracker.update(boxes, include_lost=True)`。

### LineCounter：双向竖直过线计数

`nn.LineCounter(line_x)` 根据同一 `track_id` 的前后中心点计算越线事件。一个轨迹
每个方向默认只计一次，计数器字段为 `left_to_right`、`right_to_left`、`total`。

```python
counter = nn.LineCounter(line_x=360)
events = counter.update(tracks)
for event in events:
    print(event.track_id, event.direction)
```

`event.direction` 为 `left_to_right` 或 `right_to_left`。计数线与框坐标必须使用
同一坐标系；接收默认 `nn` 检测结果时使用屏幕坐标（720x480）。

### TargetTracker：触控框选单目标跟踪

`nn.TargetTracker` 是 Sipeed 风格的单目标选择器。它不增加模型，复用检测框和
ByteTrack：触控 UI 在屏幕上拖出选择框后调用 `select()`，下一帧会在选择框中找到
目标并锁定其 `track_id`，后续持续返回同一个目标。

```python
detector = nn.load("yolov8n_det_coco80.mud", threshold=0.25)
tracker = nn.TargetTracker()

# 触控拖框结束时调用，坐标为 720x480 屏幕坐标
tracker.select(120, 80, 300, 260)
with camera.read() as frame:
    detections = detector.run(frame)
target = tracker.update(detections.boxes)
if target is not None:
    print(target.track_id, target.box, target.lost)

tracker.clear()  # 点击取消目标
```

`TargetTracker` 的 `selected_id` 表示当前锁定 ID，`selecting` 表示已框选但尚未在
画面中匹配到目标。返回的 `TargetTrack` 有 `track_id`、`box`、`age`、`missed` 和
`lost` 字段。框选区域与检测框有交集，或目标中心点在框内时完成锁定；短暂丢失会在
`max_missed` 范围内尝试恢复同一 ID。完整 LVGL 例程见 `python/dara_object_tracker.py`。

### FearTrack：框选单目标视觉跟踪

`nn.load("feartrack.mud")` 是完整的 Sipeed 风格“框一个目标就持续跟踪”接口。
它使用 `feartrack.mud`：框选时以当前帧 ROI 作为模板，随后由 VPSS 硬件裁剪
模板/搜索区域，并由 FearTrack NPU 进行两输入匹配。跟踪阶段不依赖 YOLO 检测，
因此检测模型一两帧漏检不会立即使目标丢失。

```python
from dara import camera, nn

tracker = nn.load("feartrack.mud")

# 触控拖框结束后，在同一个有效帧中初始化；坐标为 720x480 屏幕坐标。
with camera.read() as frame:
    tracker.initialize(frame, 120, 80, 300, 260)

with camera.read() as frame:
    result = tracker.run(frame)
print(result.tracked, result.confidence, result.box)
```

`VisualTrackingResult` 包含 `box`、`tracked`、`confidence`，以及
`preprocess_ms`、`inference_ms`、`output_copy_ms`、`postprocess_ms`、`total_ms`
性能字段。`tracker.reset()` 取消当前目标后可重新框选。完整 LVGL 触控应用为
`python/dara_object_tracker.py`。

## `to_screen=False` 的注意事项

```python
model = nn.load("pose_classifier.mud", to_screen=False)
with camera.read() as frame:
    raw = model.run(frame)
```

此时返回的是底层 `tdl_py` 对象：字段结构与对应算法相同，但框、点仍是 AI 帧
坐标（float）。其中人体姿态使用 `raw.keypoints.points`，手势是原始手结果列表，
人脸识别是原始人脸结果列表。只有在需要映射到非 720x480 输出时才建议使用此模式；
常规 LVGL/OSD 绘制请保留默认 `to_screen=True`。

## 旧接口兼容

历史应用中的 `camera.to_screen()` 和 `tdl_py.rgn_destroy()` 仍可使用，但它们不是
算法推理接口。新的算法应用统一通过 `nn.load(..., to_screen=True)` 取得结果，直接
使用 `Box`、`Point`、`faces`、`hands`、`classes` 等字段绘制；不要再对这些已映射到
720x480 的结果调用 `camera.to_screen()`。
