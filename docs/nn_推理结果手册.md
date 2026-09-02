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

## 语音流式算法

语音算法不经过 `nn.load()`，统一从 `dara.audio` 创建。底层由独立的
`tdl_audio.so` 处理 AI 采集、模型推理和声纹数据库；应用只使用 `dara.audio`，
不应直接依赖 `tdl_audio`。

```python
from dara import audio
```

当前公开的是 ASR、声纹和 KWS 三类算法。它们的输入统一为 **16 kHz、单声道、
16-bit little-endian PCM**。实时模式会自行打开板端麦克风；默认使用
`input_volume=40`、`points_per_frame=320`（20 ms）。不要把硬件采集周期改回
160，已验证 320 更稳定；ASR/KWS 在内部仍按 160 个采样点（10 ms）送入模型。

| 能力 | 创建方式 | 模型描述文件 |
|---|---|---|
| 流式中文 ASR | `audio.StreamingAsr()` | `/root/models/npu_zipformer_zh_14m_asr.mud` |
| 多人声纹 | `audio.SpeakerRecognizer()` | `/root/models/speaker_campplus_sv.mud` |
| 动态中文 KWS | `audio.KeywordSpotter()` | `/root/models/npu_zipformer_zh_kws.mud` |

### 两种输入模式

每个算法均可选择其一，不能在同一识别会话中混用。

1. **离线 PCM**：将已有 PCM 分块传给 `accept(pcm)`，结束时调用 `finish()` 刷出
   模型残留结果。
2. **实时麦克风**：`start()` 后在应用主循环持续调用 `read()`，退出时先
   `stop()`，再按需要调用 `finish()`。

`read()` 返回 `None` 表示设备或推理失败，应检查 `last_error`；空字符串或空列表
是一次正常的无识别结果，不能视为异常。实时读取只处理一个 20 ms 音频帧，适合直接
放入 LVGL、`asyncio` 或普通帧循环，不应使用额外阻塞线程包裹。

### Audio：基础录制、播放和回环

不使用 AI 模型时，使用 `audio.Audio`。它只控制 AI 麦克风和 AO 播放设备，不加载
`tdl_audio.so` 的 ASR/KWS/声纹模型。录音默认是 **16 kHz、单声道、16-bit PCM**，
并使用已验证稳定的 `input_volume=40`、`points_per_frame=320`；播放默认音量为 16。

最简录制后回放：

```python
from dara import audio

io = audio.Audio()
path = "/tmp/record.wav"

if not io.record_wav(path, seconds=3.0):
    raise RuntimeError(io.last_error)
if not io.play_wav(path):
    raise RuntimeError(io.last_error)
```

| 成员 | 含义 |
|---|---|
| `record_wav(path, seconds=3.0, ...)` | 录制标准 PCM WAV 文件 |
| `play_wav(path, output_volume=16)` | 播放 PCM WAV 文件 |
| `capture_pcm(seconds=3.0, ...)` | 返回内存中的 PCM `bytes`，适合离线送入 ASR、KWS 或声纹接口 |
| `loopback(seconds=3.0, ...)` | 麦克风直通扬声器，用于检查线路与音量；不要在正式应用中长期保持 |
| `set_input_volume(value)` / `set_output_volume(value)` | 设置 AI / AO 音量 |
| `input_volume()` / `output_volume()` | 读取当前音量；失败时返回 `None` |
| `status()` | 返回运行时、AI/AO 流与默认格式状态 |
| `last_error` | 任意方法返回 `False` 或 `None` 时的错误原因 |

录制 PCM 后直接做离线 ASR 的最小示例：

```python
from dara import audio

io = audio.Audio()
pcm = io.capture_pcm(seconds=3.0)
if pcm is None:
    raise RuntimeError(io.last_error)

asr = audio.StreamingAsr()
if not asr.load("/root/models/npu_zipformer_zh_14m_asr.mud"):
    raise RuntimeError(asr.last_error)
print((asr.accept(pcm) or "") + (asr.finish() or ""))
```

同一时刻 AI 设备只能由一个录制、回环或实时算法占用。调用 `record_wav()`、
`capture_pcm()`、`play_wav()`、`loopback()` 都是同步操作，完成后才会返回；UI 应用
应在工作线程执行它们，或改用下文的实时算法接口。

### StreamingAsr：实时语音识别

```python
from dara import audio

asr = audio.StreamingAsr()
if not asr.load("/root/models/npu_zipformer_zh_14m_asr.mud"):
    raise RuntimeError(asr.last_error)

if not asr.start():
    raise RuntimeError(asr.last_error)

try:
    while True:
        result = asr.read()
        if result is None:
            raise RuntimeError(asr.last_error)
        if result["text"]:
            print(result["text"], "=>", result["full_text"])
finally:
    asr.stop()
    final_text = asr.finish()
    if final_text:
        print("final:", final_text)
```

| 成员 | 含义 |
|---|---|
| `load(model_spec)` | 加载 Zipformer encoder/decoder/joiner 模型 |
| `start(input_volume=40, points_per_frame=320, timeout_ms=1000)` | 打开麦克风并重置当前识别句子 |
| `read()` | 返回 `{"text", "full_text", "timestamp", "sequence"}`；`text` 是本次新增文本，`full_text` 是本会话累积文本 |
| `accept(pcm)` | 离线输入一个 PCM 块，返回本块新增文本 |
| `finish()` | 刷出末尾未完成文本；实时或离线会话结束时调用 |
| `stop()` / `reset()` | 关闭麦克风 / 清空当前句子但保留模型 |
| `initialized`、`listening`、`text`、`last_error` | 只读状态 |

离线文件示例：

```python
asr = audio.StreamingAsr()
asr.load("/root/models/npu_zipformer_zh_14m_asr.mud")
with open("/tmp/input.pcm", "rb") as source:
    while pcm := source.read(3200):      # 100 ms，大小可按需调整
        print(asr.accept(pcm) or "", end="")
print(asr.finish() or "")
```

### SpeakerRecognizer：注册、识别和多人声纹库

声纹库在当前对象内存中维护，可用 `save_database()` 保存为一个文件，重启后用
`load_database()` 恢复。标签就是普通字符串，支持多人；同名 `enroll()` 会覆盖该
用户的原有特征。

```python
from dara import audio

speaker = audio.SpeakerRecognizer()
speaker.load("/root/models/speaker_campplus_sv.mud")
speaker.load_database("/root/speakers.db")  # 文件不存在时可忽略或自行判断

# 非阻塞录入：在原有 UI 主循环里持续 poll，不要 sleep 等待。
speaker.begin_enroll("用户1", seconds=4.0)
while speaker.capturing:
    event = speaker.poll()
    if event is None:
        raise RuntimeError(speaker.last_error)
    if event["done"]:
        print("enrolled", event["label"])

speaker.save_database("/root/speakers.db")
print(speaker.labels())

# 非阻塞识别当前说话人。
speaker.begin_identify(seconds=3.0, threshold=0.60)
while speaker.capturing:
    event = speaker.poll()
    if event and event["done"]:
        print(event["label"], event["score"], event["matched"])
```

| 成员 | 含义 |
|---|---|
| `load(model_spec)` | 加载 CAMPPlus 声纹模型 |
| `enroll(label, pcm)` | 从离线 PCM 提取特征，并增加或覆盖该标签 |
| `recognize(pcm, threshold=0.60)` | 返回最佳 `{"label", "score", "matched"}`，提取失败时为 `None` |
| `verify(label, pcm, threshold=0.60)` | 仅对一个标签验证，返回相同匹配字段 |
| `begin_enroll(label, seconds=3.0, ...)` | 开始非阻塞麦克风注册 |
| `begin_identify(seconds=3.0, threshold=0.60, ...)` | 开始非阻塞多人识别 |
| `begin_verify(label, seconds=3.0, threshold=0.60, ...)` | 开始非阻塞指定人验证 |
| `poll()` | 返回 `done`、`progress`、`timestamp`、`sequence`；完成时额外带 `label`、`score`、`matched` |
| `cancel()` | 取消当前采集 |
| `save_database(path)` / `load_database(path)` | 持久化或恢复多人声纹库 |
| `labels()` / `clear()` | 取已注册标签 / 清空内存库 |

`score` 是特征余弦相似度而不是概率。`0.60` 是当前默认起点，实际阈值应在本机麦克风、
距离和环境噪声下通过多人录音测试后确定。每位用户应在相同使用距离下注册，且建议注册
时连续说话 3 至 4 秒。

### KeywordSpotter：运行时中文关键词注册

KWS 模型实际匹配的是带声调拼音 token。高层 `KeywordSpotter.register()` 会将中文
转换为模型 token，因此应用只传中文文本和独立阈值即可。中文转换依赖系统 Alpine 包
`py3-pypinyin`，它由完整 rootfs 构建安装；Dara wheel 不再内置该词典。

```python
from dara import audio

kws = audio.KeywordSpotter()
if not kws.load("/root/models/npu_zipformer_zh_kws.mud", beam_width=6):
    raise RuntimeError(kws.last_error)

# 必须在 start() 前注册；同名注册会替换原配置。
assert kws.register("竞业达", confidence=0.15), kws.last_error
assert kws.register("文森特卡索", confidence=0.10), kws.last_error
print(kws.registered_keywords())

assert kws.start(), kws.last_error
try:
    while True:
        hits = kws.read()
        if hits is None:
            raise RuntimeError(kws.last_error)
        for hit in hits:                 # [] 是正常的未命中状态
            if hit["triggered"]:
                print(hit["name"], hit["confidence"])
finally:
    kws.stop()
    for hit in kws.finish() or []:
        print("final", hit["name"])
```

| 成员 | 含义 |
|---|---|
| `load(model_spec, keywords_path=None, threshold=-1.0, beam_width=2)` | 加载 KWS 模型；新代码省略 `keywords_path`，旧关键词文件模式仅为兼容保留 |
| `register(text, confidence=0.15, name=None)` | 注册中文关键词；`name` 可为展示名称，默认使用 `text` |
| `unregister(name)` / `registered_keywords()` | 移除关键词 / 返回当前已注册名称 |
| `start(...)` / `read()` / `stop()` / `finish()` | 实时麦克风生命周期；`read()` 返回命中列表 |
| `accept(pcm)` | 离线输入 PCM，返回新命中列表 |
| `scores()` | 返回所有关键词的当前匹配信息，用于调试阈值 |
| `reset()` | 重置模型流状态；不要在每次命中后调用，否则会降低连续识别效果 |
| `initialized`、`listening`、`last_error` | 只读状态 |

每个命中元素包含：`name`、`confidence`、`threshold`、`matched_tokens`、
`total_tokens`、`matched_text`、`end_time_seconds`、`complete`、`triggered`。
只有 `triggered=True` 才应执行业务动作。关键词注册、移除只允许在未监听状态执行；
运行中修改会返回 `False`，原因见 `last_error`。

### 部署与排错

- 使用完整镜像时，`py3-pypinyin` 由 `jyd_alpine_rootfs/apk/world.python` 提供。
  单独升级 Dara wheel 到旧镜像前，需确认：

  ```sh
  python3 -c 'import pypinyin; print(pypinyin.__file__)'
  ```

- 无论使用哪种算法，`load()`、`start()`、`begin_*()` 返回 `False` 时均先打印
  `last_error`，不要继续调用 `read()` 或 `poll()`。
- 同一时刻只能有一个应用占用 AI 麦克风。结束应用时务必调用 `stop()` 或
  `cancel()`，避免下一应用打开 AI 设备失败。
- 固件路径已由原生扩展统一处理，应用不需要设置
  `BMRUNTIME_USING_FIRMWARE`，也不需要显式导入 `tdl_audio.so`。

## 小核 RGB LED

RGB 灯由小核驱动，不需要大核直接操作 GPIO。当前 `tdl_py.RgbLed` 已封装了大核到
小核的 IPC 调用，默认控制 14 颗可寻址 RGB LED。构造前调用 `tdl_py.init()`，它会确保
双核 MMF/IPC 运行时已初始化。

```python
import time
import tdl_py

tdl_py.init()
led = tdl_py.RgbLed(pixel_count=14)

# 所有灯显示绿色；set_all 只更新缓存，show 才真正输出。
if not led.set_all(0, 80, 0) or not led.show():
    raise RuntimeError("RGB LED IPC command failed")
time.sleep(1)

# 第 0 颗显示红色，其余保持绿色。
if not led.set_pixel(0, 255, 0, 0) or not led.show():
    raise RuntimeError("RGB LED IPC command failed")
time.sleep(1)

led.clear()  # 清缓存并立即熄灭
```

| 成员 | 含义 |
|---|---|
| `tdl_py.RgbLed(pixel_count=14)` | 创建并启用小核 RGB LED 服务；`pixel_count` 范围为 1 至 14 |
| `set_all(r, g, b)` | 设置全部灯的 RGB 缓存值，三个分量均为 0 至 255 |
| `set_pixel(index, r, g, b)` | 设置一颗灯，索引为 0 至 `pixel_count - 1` |
| `brightness(value)` | 设置 0 至 255 的全局亮度并重新缩放缓存颜色；之后调用 `show()` |
| `show()` | 将当前缓存一次性提交给小核，避免逐颗更新出现闪烁 |
| `clear()` | 清空并立即熄灭所有 RGB 灯 |

例如红色闪烁三次：

```python
import time
import tdl_py

tdl_py.init()
led = tdl_py.RgbLed()
led.brightness(64)
for _ in range(3):
    led.set_all(255, 0, 0)
    led.show()
    time.sleep(0.2)
    led.clear()
    time.sleep(0.2)
```

该类没有动画线程；闪烁、跑马灯等效果由上层定时调用 `set_pixel()` 和 `show()` 实现。
应用退出前调用 `clear()`，避免灯带停留在最后一个颜色。
