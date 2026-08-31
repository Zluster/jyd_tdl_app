"""jyd.nn：.mud 模型统一加载与推理。

nn.load(spec) 自动识别模型族并返回带 run(frame) 的 Model：

    detection        目标检测        run -> Result（.boxes / .label_of）
    classification   分类            run -> Result（.classes）
    keypoint         关键点/姿态     run -> KeypointResult（.points）
    seg              实例分割        run -> SegResult（.instances）
    ocr              车牌/OCR        run -> Result（.boxes / .attributes / .text）
    face_dense       人脸稠密关键点  run -> [(Box, [Point]), ...]（两级管线）
    face_recognition 人脸识别        run -> FaceRecognitionResult（.faces）
    hand_gesture     手势识别        run -> HandGestureResult（.hands）
    pose_classifier  姿态分类        run -> PoseResult（.points / .label）
    self_learning    自学习分类      run -> SelfLearningResult（.classes）
    face_emotion      人脸情绪属性    run -> FaceEmotionResult（.faces）

结果坐标默认已映射到 720x480 屏幕坐标系（仿 launcher/apps/ai 的
letterbox CoordMap：推理帧与屏幕显示的是同一 1600x1200 sensor 画面的
等比嵌入，去帧黑边、按内容归一、再加屏幕黑边；ai 640x640 帧退化为
x+40 / y-80 平移），可直接画 LVGL/OSD。转换产物是纯 Python 副本，
出 with 块后仍有效。nn.load(spec, to_screen=False) 返回 tdl_py 原始
结果（帧坐标系）。

族判定与 C++ detectFamily 一致（.mud [extra] 的 task 优先、model_type
前缀兜底）；管线复制改造自 launcher/apps/ai/app.py（按约定不动老代码）。

用法（frame 推荐来自 ai 通道，run 必须在 with 块内调用）：

    from jyd import camera, nn
    model = nn.load("yolov8n_det_coco80.mud")   # 短名解析到默认模型目录
    with camera.read() as frame:                # ai 通道 zero-copy Frame
        r = model.run(frame)
    for box in r.boxes:                         # 已是屏幕坐标
        print(box.x1, box.y1, r.label_of(box.class_id))
"""

import configparser
import os

import tdl_py

#: 短名（非绝对路径且当前目录不存在）默认在此目录下解析
DEFAULT_MODEL_DIR = "/root/models/"

#: 坐标映射的源（sensor 原生）与目标（屏幕）尺寸，取双系统布局常量
_SRC_W, _SRC_H = 1600, 1200            # 1600x1200
_SCREEN_W, _SCREEN_H = tdl_py.SCREEN_WIDTH, tdl_py.SCREEN_HEIGHT  # 720x480


def _letterbox(frame_w, frame_h, src_w, src_h):
    """src 等比嵌入 frame_w×frame_h 加黑边：返回 (ox, oy, 内容宽, 内容高)。"""
    fit = min(frame_w / src_w, frame_h / src_h)
    cw, ch = src_w * fit, src_h * fit
    return (frame_w - cw) / 2.0, (frame_h - ch) / 2.0, cw, ch


def _make_map(frame_w, frame_h):
    """帧 -> 屏幕坐标映射（仿 launcher/apps/ai 的 CoordMap）。

    推理帧与屏幕显示的都是同一 sensor 画面的等比嵌入：ai 640x640 上下
    黑边（内容 640x480，oy=80），屏幕 720x480 左右黑边（内容 640x480，
    ox=40）。映射 = 去帧黑边按内容归一，再加屏幕黑边；ai 帧数值下退化
    为 x'=x+40 / y'=y-80，720x480 帧（live/rgb）则为恒等。"""
    ox_f, oy_f, cw_f, ch_f = _letterbox(frame_w, frame_h, _SRC_W, _SRC_H)
    ox_s, oy_s, cw_s, ch_s = _letterbox(_SCREEN_W, _SCREEN_H, _SRC_W, _SRC_H)

    def m(x, y):
        return (int((x - ox_f) / cw_f * cw_s + ox_s),
                int((y - oy_f) / ch_f * ch_s + oy_s))
    return m


def _screen_to_frame(frame_w, frame_h, x, y):
    """720x480 屏幕坐标反算为当前推理帧坐标。"""
    ox_f, oy_f, cw_f, ch_f = _letterbox(frame_w, frame_h, _SRC_W, _SRC_H)
    ox_s, oy_s, cw_s, ch_s = _letterbox(_SCREEN_W, _SCREEN_H, _SRC_W, _SRC_H)
    return (int((float(x) - ox_s) / cw_s * cw_f + ox_f),
            int((float(y) - oy_s) / ch_s * ch_f + oy_f))


# ---- 屏幕坐标系结果结构（tdl_py 只读结果的纯 Python 转换副本） ----

class Point:
    """关键点/五官点（屏幕坐标系）。"""
    __slots__ = ("x", "y", "score")

    def __init__(self, x, y, score):
        self.x = x
        self.y = y
        self.score = score

    def __repr__(self):
        return "<jyd.nn.Point x=%d y=%d score=%.2f>" % (
            self.x, self.y, self.score)


class Box:
    """检测框（屏幕坐标系）。landmarks：人脸检测器附带的五官点。"""
    __slots__ = ("x1", "y1", "x2", "y2", "score", "class_id", "landmarks")

    def __init__(self, x1, y1, x2, y2, score, class_id, landmarks):
        self.x1 = x1
        self.y1 = y1
        self.x2 = x2
        self.y2 = y2
        self.score = score
        self.class_id = class_id
        self.landmarks = landmarks

    @property
    def width(self):
        return self.x2 - self.x1

    @property
    def height(self):
        return self.y2 - self.y1

    def __repr__(self):
        return "<jyd.nn.Box (%d,%d)-(%d,%d) class=%d score=%.2f>" % (
            self.x1, self.y1, self.x2, self.y2, self.class_id, self.score)


class Result:
    """detection / classification / ocr 结果，字段与 AlgorithmResult 对齐。

    boxes 已转屏幕坐标；classes/labels/text/attributes 无坐标，原样保留
    （OCR 的 attributes 与 boxes 顺序一致）。"""

    def __init__(self, boxes, classes, labels, text, attributes):
        self.boxes = boxes
        self.classes = classes
        self.labels = labels
        self.text = text
        self.attributes = attributes

    def label_of(self, class_id):
        """class_id 的标签串，越界时返回 id 的字符串形式。"""
        if 0 <= class_id < len(self.labels):
            return self.labels[class_id]
        return str(class_id)


class KeypointResult:
    """keypoint 族结果；points 已转屏幕坐标，width/height 为屏幕尺寸。"""

    def __init__(self, points):
        self.width = _SCREEN_W
        self.height = _SCREEN_H
        self.points = points


class Face:
    """一个人脸识别结果，box 为屏幕坐标。"""
    __slots__ = ("box", "name", "score", "matched", "class_id", "points")

    def __init__(self, box, name, score, matched, class_id, points):
        self.box = box
        self.name = name
        self.score = score
        self.matched = matched
        self.class_id = class_id
        self.points = points


class FaceRecognitionResult:
    """人脸识别结果；faces 为纯 Python 副本，坐标已映射到屏幕。"""
    def __init__(self, faces):
        self.faces = faces


class FaceEmotion:
    """一个人脸的情绪与属性结果；box 为屏幕坐标。"""
    __slots__ = ("box", "emotion", "emotion_id", "emotion_score",
                 "detection_score", "gender", "gender_label", "age",
                 "age_years", "glasses", "has_glasses")

    def __init__(self, box, emotion, emotion_id, emotion_score,
                 detection_score, gender, gender_label, age, age_years,
                 glasses, has_glasses):
        self.box = box
        self.emotion = emotion
        self.emotion_id = emotion_id
        self.emotion_score = emotion_score
        self.detection_score = detection_score
        self.gender = gender
        self.gender_label = gender_label
        self.age = age
        self.age_years = age_years
        self.glasses = glasses
        self.has_glasses = has_glasses


class FaceEmotionResult:
    """人脸情绪识别结果；faces 为已映射到屏幕的纯 Python 副本。"""
    def __init__(self, faces):
        self.faces = faces


class Hand:
    """一只手的检测框、21 点和分类标签，均为屏幕坐标。"""
    __slots__ = ("box", "keypoints", "label", "gesture", "score")

    def __init__(self, box, keypoints, label, gesture, score):
        self.box = box
        self.keypoints = keypoints
        self.label = label
        self.gesture = gesture
        self.score = score


class HandGestureResult:
    """手势识别结果；hands 为纯 Python 副本。"""
    def __init__(self, hands):
        self.hands = hands


class PoseResult(KeypointResult):
    """人体姿态分类结果；points 已映射到屏幕，label 是平滑后的类别。"""
    def __init__(self, points, label, raw_label, confidence, history_size,
                 keypoint_ms, total_ms):
        super().__init__(points)
        self.label = label
        self.raw_label = raw_label
        self.confidence = confidence
        self.history_size = history_size
        self.keypoint_ms = keypoint_ms
        self.total_ms = total_ms


class LearningClass:
    """自学习分类的一项类别相似度。"""
    __slots__ = ("label", "score", "sample_count")

    def __init__(self, label, score, sample_count):
        self.label = label
        self.score = score
        self.sample_count = sample_count


class SelfLearningResult:
    """自学习分类结果；classes 按相似度从高到低排列。"""
    def __init__(self, classes, feature_dim):
        self.classes = classes
        self.feature_dim = feature_dim


class InstanceSegment:
    """seg 单实例：box + outline（多边形轮廓），均为屏幕坐标。"""
    __slots__ = ("box", "outline")

    def __init__(self, box, outline):
        self.box = box
        self.outline = outline


class SegResult:
    """seg 族结果；instances 已转屏幕坐标，width/height 为屏幕尺寸。"""

    def __init__(self, instances):
        self.width = _SCREEN_W
        self.height = _SCREEN_H
        self.instances = instances


def _map_point(m, p):
    x, y = m(p.x, p.y)
    return Point(x, y, p.score)


def _map_box(m, b):
    x1, y1 = m(b.x1, b.y1)
    x2, y2 = m(b.x2, b.y2)
    return Box(x1, y1, x2, y2, b.score, b.class_id,
               [_map_point(m, p) for p in b.landmarks])


def _convert(family, raw, m):
    """tdl_py 原始结果 -> 屏幕坐标系纯 Python 副本（按族分派）。"""
    if family == "keypoint":
        return KeypointResult([_map_point(m, p) for p in raw.points])
    if family == "face_recognition":
        return FaceRecognitionResult([
            Face(_map_box(m, item.box), item.name, item.score, item.matched,
                 item.class_id, [_map_point(m, p) for p in item.points])
            for item in raw])
    if family == "face_emotion":
        return FaceEmotionResult([
            FaceEmotion(_map_box(m, item.box), item.emotion, item.emotion_id,
                        item.emotion_score, item.detection_score, item.gender,
                        item.gender_label, item.age, item.age_years,
                        item.glasses, item.has_glasses)
            for item in raw])
    if family == "hand_gesture":
        return HandGestureResult([
            Hand(_map_box(m, item.box),
                 [_map_point(m, p) for p in
                  getattr(item.keypoints, "points", item.keypoints)], item.label,
                 item.gesture, item.score)
            for item in raw])
    if family == "pose_classifier":
        return PoseResult([_map_point(m, p) for p in raw.keypoints.points],
                          raw.label, raw.raw_label, raw.confidence,
                          raw.history_size, raw.keypoint_ms, raw.total_ms)
    if family == "self_learning":
        return SelfLearningResult(
            [LearningClass(item.label, item.score, item.sample_count)
             for item in raw.classes], raw.feature_dim)
    if family == "seg":
        return SegResult(
            [InstanceSegment(_map_box(m, i.box),
                             [_map_point(m, p) for p in i.outline])
             for i in raw.instances])
    if family == "face_dense":
        return [(_map_box(m, box), [_map_point(m, p) for p in pts])
                for box, pts in raw]
    return Result([_map_box(m, b) for b in raw.boxes], list(raw.classes),
                  list(raw.labels), raw.text, list(raw.attributes))


def _detect_family(spec):
    """照抄 C++ detectFamily：task 字段优先，model_type 前缀兜底；
    face_dense_landmark 是两级管线（人脸检测+稠密关键点），单独一族。"""
    cfg = configparser.ConfigParser()
    cfg.read(spec)
    desc = dict(cfg.items("extra")) if cfg.has_section("extra") else {}
    task = desc.get("task", "").upper()
    mt = desc.get("model_type", "").upper()
    runtime = desc.get("runtime", "").upper()
    if mt == "FACE_LANDMARKS_DENSE" or runtime == "FACE_DENSE_LANDMARK":
        return "face_dense"
    if task in ("FACE_RECOGNITION", "FACE_RECOGNIZE"):
        return "face_recognition"
    if task in ("FACE_EMOTION", "FACE_ATTRIBUTE"):
        return "face_emotion"
    if task in ("HAND_GESTURE", "GESTURE"):
        return "hand_gesture"
    if task in ("POSE_CLASSIFICATION", "POSE_CLASSIFY"):
        return "pose_classifier"
    if task in ("FEATURE", "EMBEDDING"):
        return "self_learning"
    if task in ("TRACKING", "TRACK") or mt.startswith("TRACKING_"):
        return "tracking"
    if task in ("CLASSIFY", "CLASSIFICATION"):
        return "classification"
    if task in ("KEYPOINT", "LANDMARK"):
        return "keypoint"
    if task in ("SEGMENTATION", "INSTANCE_SEGMENTATION"):
        return "seg"
    if task == "OCR":
        return "ocr"
    if task in ("DETECT", "DETECTION"):
        return "detection"
    if mt.startswith(("PP_OCR", "PLATE_", "LPR")):
        return "ocr"
    if "SEG" in mt:
        return "seg"
    if mt.startswith("KEYPOINT") or "POSE" in mt:
        return "keypoint"
    if mt.startswith("CLS") or "CLASSIFIER" in mt:
        return "classification"
    return "detection"


class _SinglePipeline:
    """单模型族适配：load/run/reset 直接代理到底层算法对象。"""

    def __init__(self, algo, runner):
        self._algo = algo
        self._run = runner

    def load(self, spec, firmware):
        self._algo.load(spec, firmware)

    def run(self, frame):
        return self._run(frame)

    def reset(self):
        self._algo.reset()


class _FaceDensePipeline:
    """face_dense 两级管线：Detector 出脸框，FaceDenseLandmark 逐脸出点。

    dense=False 时跳过第二级（不加载稠密模型），只出 SCRFD 自带的
    五点（box.landmarks：双眼/鼻尖/嘴角）。"""

    def __init__(self, threshold, face_spec, expand, dense=True):
        self._det = tdl_py.Detector()
        self._lmk = tdl_py.FaceDenseLandmark()
        self._threshold = threshold
        self._face_spec = face_spec
        self._expand = expand
        self._dense = dense

    def load(self, spec, firmware):
        if self._dense:
            self._lmk.load(spec, firmware)
        self._det.load(self._face_spec, firmware)

    def run(self, frame):
        r = self._det.detect(frame, self._threshold)
        faces = []
        for box in r.boxes[:2]:   # 稠密点数据量大，最多跟 2 张脸
            if self._dense:
                faces.append((box, self._lmk.estimate(frame, box, self._expand)))
            else:
                faces.append((box, box.landmarks))
        return faces

    def reset(self):
        for a in (self._det, self._lmk):
            try:
                a.reset()
            except Exception:
                pass


def _require_binding(name):
    value = getattr(tdl_py, name, None)
    if value is None:
        raise RuntimeError(
            "当前 tdl_py.so 不包含 %s；请更新包含新增算法绑定的 tdl_py.so"
            % name)
    return value


def _extra_path(spec, value, default):
    """读取管线 .mud 中的相对模型路径，缺省使用同目录的默认短名。"""
    value = value or default
    if os.path.isabs(value):
        return value
    local = os.path.join(os.path.dirname(spec), value)
    return local if os.path.exists(local) else _resolve_spec(value)


def _extra(spec):
    cfg = configparser.ConfigParser()
    cfg.read(spec)
    return dict(cfg.items("extra")) if cfg.has_section("extra") else {}


class _FaceRecognitionPipeline:
    def __init__(self, detector_model, feature_model, threshold, match_threshold,
                 max_faces):
        self._algo = _require_binding("FaceRecognizer")()
        self._detector_model = detector_model
        self._feature_model = feature_model
        self._threshold = threshold
        self._match_threshold = match_threshold
        self._max_faces = max_faces

    def load(self, spec, firmware):
        self._algo.load(self._detector_model, self._feature_model,
                        self._threshold, self._match_threshold,
                        self._max_faces, firmware)

    def run(self, frame):
        return self._algo.recognize(frame, conf_th=self._threshold,
                                    match_th=self._match_threshold)

    def reset(self):
        # FaceRecognizer has no reset binding. Keep the runtime until process
        # exit; this also avoids the CV184X final-BMRT teardown issue.
        pass

    def enroll(self, frame, name):
        self._algo.enroll(frame, name)

    def names(self):
        return list(self._algo.names())

    def save(self, path):
        self._algo.save_faces(path)

    def load_faces(self, path):
        self._algo.load_faces(path)

    def clear(self):
        self._algo.clear()


class _FaceEmotionPipeline:
    """SCRFD 人脸检测 + 人脸属性/情绪模型的两级在线管线。"""
    def __init__(self, detector_model, attribute_model, threshold, max_faces):
        self._algo = _require_binding("FaceEmotionRecognizer")()
        self._detector_model = detector_model
        self._attribute_model = attribute_model
        self._threshold = threshold
        self._max_faces = max_faces

    def load(self, spec, firmware):
        self._algo.load(self._detector_model, self._attribute_model,
                        self._threshold, self._max_faces, firmware)

    def run(self, frame):
        return self._algo.recognize(frame)

    def reset(self):
        # 当前底层绑定没有 reset；与 FaceRecognizer 保持相同的析构策略。
        pass


class _HandGesturePipeline:
    def __init__(self, detector_model, keypoint_model, threshold, max_hands):
        self._algo = _require_binding("HandGestureRecognizer")()
        self._detector_model = detector_model
        self._keypoint_model = keypoint_model
        self._threshold = threshold
        self._max_hands = max_hands

    def load(self, spec, firmware):
        self._algo.load(self._detector_model, self._keypoint_model,
                        self._threshold, max_hands=self._max_hands,
                        firmware=firmware)

    def run(self, frame):
        return self._algo.recognize(frame)

    def reset(self):
        self._algo.reset()


class _PoseClassifierPipeline:
    def __init__(self, model, threshold, ema_alpha, smooth_frames):
        self._algo = _require_binding("PoseClassifier")()
        self._model = model
        self._threshold = threshold
        self._ema_alpha = ema_alpha
        self._smooth_frames = smooth_frames

    def load(self, spec, firmware):
        self._algo.load(self._model, self._threshold, self._ema_alpha,
                        self._smooth_frames, firmware)

    def run(self, frame):
        return self._algo.classify(frame)

    def reset(self):
        self._algo.reset()


class _SelfLearningPipeline:
    def __init__(self, top_k):
        self._algo = _require_binding("SelfLearningClassifier")()
        self._top_k = top_k

    def load(self, spec, firmware):
        self._algo.load(spec, firmware)

    def run(self, frame):
        return self._algo.classify(frame, self._top_k)

    def reset(self):
        # Same ownership rule as FaceRecognizer: the binding intentionally has
        # no reset because its final runtime release is unsafe on this target.
        pass

    def add_sample(self, label, image):
        self._algo.add_sample(label, image)

    def add_frame(self, label, frame):
        self._algo.add_frame(label, frame)

    def save(self, path):
        self._algo.save(path)

    def load_bank(self, path):
        self._algo.load_bank(path)

    def clear(self):
        self._algo.clear()

    @property
    def sample_count(self):
        return self._algo.sample_count

    @property
    def class_count(self):
        return self._algo.class_count


class _FearTrackPipeline:
    """FearTrack 单目标跟踪管线；由 ``nn.load('feartrack.mud')`` 创建。"""
    def __init__(self):
        self._tracker = _require_binding("SingleObjectTracker")()
        self._spec = None
        self._firmware = ""

    def load(self, spec, firmware):
        self._spec = spec
        self._firmware = firmware
        self._tracker.load(spec, firmware)

    def initialize(self, frame, x1, y1, x2, y2):
        # nn 的公开坐标始终是屏幕坐标；底层 FearTrack 接受源帧坐标。
        fx1, fy1 = _screen_to_frame(frame.width, frame.height, x1, y1)
        fx2, fy2 = _screen_to_frame(frame.width, frame.height, x2, y2)
        x1, x2 = sorted((fx1, fx2))
        y1, y2 = sorted((fy1, fy2))
        if x2 <= x1 or y2 <= y1:
            raise ValueError("selection box must have positive size")
        self._tracker.initialize(frame, Box(x1, y1, x2, y2, 1.0, 0, []))

    def run(self, frame):
        return self._tracker.track(frame)

    def reset(self):
        # C++ reset() 会关闭 BMRT 会话；重新 load 才能再次 initialize。
        self._tracker.reset()
        self._tracker.load(self._spec, self._firmware)

    @property
    def ready(self):
        return self._tracker.ready


class Model:
    """已加载的模型管线。run(frame) 推理，reset() 卸载（可选）。"""

    def __init__(self, spec, family, pipeline, to_screen=True):
        self.spec = spec
        self.family = family
        self._pipeline = pipeline
        self._to_screen = to_screen
        self._maps = {}   # (frame_w, frame_h) -> 坐标映射函数

    def run(self, frame):
        """推理一帧。frame 是 zero-copy 引用，本调用必须在
        `with cam.read() as frame:` 块内完成。

        默认返回屏幕坐标系（720x480）的纯 Python 副本，出 with 块后仍
        有效；load(..., to_screen=False) 时返回 tdl_py 原始结果（帧坐
        标系）。"""
        raw = self._pipeline.run(frame)
        if not self._to_screen:
            return raw
        key = (frame.width, frame.height)
        m = self._maps.get(key)
        if m is None:
            m = self._maps[key] = _make_map(*key)
        if self.family == "tracking":
            return VisualTrackingResult(raw, m)
        return _convert(self.family, raw, m)

    def reset(self):
        """显式卸载模型（NPU 资源）。进程退出时操作系统会兜底回收。"""
        self._pipeline.reset()

    def initialize(self, frame, x1, y1, x2, y2):
        """用屏幕坐标框选一个 FearTrack 目标；仅 feartrack.mud 可用。"""
        if self.family != "tracking":
            raise RuntimeError("initialize() 仅适用于 feartrack.mud")
        self._pipeline.initialize(frame, x1, y1, x2, y2)

    @property
    def ready(self):
        """FearTrack 是否已经框选并建立了目标模板。"""
        if self.family != "tracking":
            raise RuntimeError("ready 仅适用于 feartrack.mud")
        return self._pipeline.ready

    # ---- 多阶段/有状态算法的统一操作 ----

    def enroll(self, frame, name):
        """向人脸识别模型录入当前最大人脸。仅 face_recognition 可用。"""
        if self.family != "face_recognition":
            raise RuntimeError("enroll() 仅适用于 face_recognition.mud")
        self._pipeline.enroll(frame, name)

    def names(self):
        """返回已录入人脸名称。仅 face_recognition 可用。"""
        if self.family != "face_recognition":
            raise RuntimeError("names() 仅适用于 face_recognition.mud")
        return self._pipeline.names()

    def save_faces(self, path):
        if self.family != "face_recognition":
            raise RuntimeError("save_faces() 仅适用于 face_recognition.mud")
        self._pipeline.save(path)

    def load_faces(self, path):
        if self.family != "face_recognition":
            raise RuntimeError("load_faces() 仅适用于 face_recognition.mud")
        self._pipeline.load_faces(path)

    def add_sample(self, label, image):
        """把一张图片文件加入自学习类别。仅 feature .mud 可用。

        当前底层绑定只支持图片路径样本；实时 camera Frame 采样接口将在
        tdl_py 补回 add_frame 后开放。"""
        if self.family != "self_learning":
            raise RuntimeError("add_sample() 仅适用于 feature .mud")
        self._pipeline.add_sample(label, image)

    def add_frame(self, label, frame):
        """把当前完整相机帧加入自学习类别。仅 feature .mud 可用。"""
        if self.family != "self_learning":
            raise RuntimeError("add_frame() 仅适用于 feature .mud")
        self._pipeline.add_frame(label, frame)

    def save_bank(self, path):
        if self.family != "self_learning":
            raise RuntimeError("save_bank() 仅适用于 feature .mud")
        self._pipeline.save(path)

    def load_bank(self, path):
        if self.family != "self_learning":
            raise RuntimeError("load_bank() 仅适用于 feature .mud")
        self._pipeline.load_bank(path)

    def clear(self):
        """清空人脸库或自学习样本库。"""
        if self.family not in ("face_recognition", "self_learning"):
            raise RuntimeError("clear() 仅适用于人脸识别或自学习模型")
        self._pipeline.clear()

    @property
    def sample_count(self):
        if self.family != "self_learning":
            raise RuntimeError("sample_count 仅适用于 feature .mud")
        return self._pipeline.sample_count

    @property
    def class_count(self):
        if self.family != "self_learning":
            raise RuntimeError("class_count 仅适用于 feature .mud")
        return self._pipeline.class_count

    def __repr__(self):
        return "<jyd.nn.Model %s (%s)>" % (os.path.basename(self.spec),
                                           self.family)


def _copy_box(box):
    return Box(box.x1, box.y1, box.x2, box.y2, box.score, box.class_id,
               [_map_point(lambda x, y: (x, y), point)
                for point in getattr(box, "landmarks", [])])


class TrackedObject:
    """一个稳定的多目标轨迹；box 坐标系与传给 tracker.update() 的框一致。"""
    __slots__ = ("track_id", "box", "age", "missed", "previous_center_x",
                 "center_x", "center_y")

    def __init__(self, track_id, box, age, missed, previous_center_x,
                 center_x, center_y):
        self.track_id = track_id
        self.box = box
        self.age = age
        self.missed = missed
        self.previous_center_x = previous_center_x
        self.center_x = center_x
        self.center_y = center_y


class ObjectTracker:
    """ByteTrack 目标跟踪器。

    update() 可直接接收 detection 的 result.boxes。低分检测用于维持既有
    轨迹，高分检测才创建新 ID；返回的对象默认只包含当前帧匹配成功的轨迹。
    """
    def __init__(self, high_score=0.45, low_score=0.15,
                 iou_threshold=0.30, max_missed=30):
        self._tracker = _require_binding("MultiObjectTracker")(
            high_score, low_score, iou_threshold, max_missed)

    def update(self, boxes, include_lost=False):
        tracks = self._tracker.update(boxes)
        out = []
        for track in tracks:
            if not include_lost and track.missed:
                continue
            out.append(TrackedObject(track.id, _copy_box(track.box), track.age,
                                     track.missed, track.previous_center_x,
                                     track.center_x, track.center_y))
        return out

    def reset(self):
        self._tracker.reset()


class VisualTrackingResult:
    """FearTrack 的一帧结果；box 已映射为 720x480 屏幕坐标。"""
    __slots__ = ("box", "confidence", "tracked", "response_x",
                 "response_y", "preprocess_ms", "inference_ms",
                 "output_copy_ms", "postprocess_ms", "total_ms")

    def __init__(self, raw, mapper):
        self.box = _map_box(mapper, raw.box)
        self.confidence = raw.confidence
        self.tracked = raw.tracked
        self.response_x = raw.response_x
        self.response_y = raw.response_y
        self.preprocess_ms = raw.preprocess_ms
        self.inference_ms = raw.inference_ms
        self.output_copy_ms = raw.output_copy_ms
        self.postprocess_ms = raw.postprocess_ms
        self.total_ms = raw.total_ms


class VisualObjectTracker:
    """Sipeed 风格的 FearTrack 单目标视觉跟踪器。

    ``initialize`` 接收应用显示用的 720x480 框选坐标，并从当前帧提取
    模板；之后 ``track`` 不依赖检测模型，可在检测器短暂漏检时持续跟踪。
    """
    def __init__(self, model_spec="feartrack.mud", firmware=""):
        self._tracker = _require_binding("SingleObjectTracker")()
        self._model_spec = _resolve_spec(model_spec)
        self._firmware = firmware
        self._tracker.load(self._model_spec, self._firmware)

    def initialize(self, frame, x1, y1, x2, y2):
        """用屏幕坐标 ROI 初始化模板；必须在 camera.read() 块内调用。"""
        fx1, fy1 = _screen_to_frame(frame.width, frame.height, x1, y1)
        fx2, fy2 = _screen_to_frame(frame.width, frame.height, x2, y2)
        x1, x2 = sorted((fx1, fx2))
        y1, y2 = sorted((fy1, fy2))
        if x2 <= x1 or y2 <= y1:
            raise ValueError("selection box must have positive size")
        self._tracker.initialize(frame, Box(x1, y1, x2, y2, 1.0, 0, []))

    def track(self, frame):
        """跟踪当前帧，返回 VisualTrackingResult；必须在 with 块内调用。"""
        raw = self._tracker.track(frame)
        return VisualTrackingResult(raw, _make_map(frame.width, frame.height))

    run = track

    def reset(self):
        """Discard the target and reload the underlying FearTrack session."""
        self._tracker.reset()
        self._tracker.load(self._model_spec, self._firmware)

    @property
    def ready(self):
        return self._tracker.ready


class TargetTrack:
    """当前被用户选中的单个目标。"""
    __slots__ = ("track_id", "box", "age", "missed", "lost", "selected")

    def __init__(self, track, selected=True):
        self.track_id = track.track_id
        self.box = track.box
        self.age = track.age
        self.missed = track.missed
        self.lost = bool(track.missed)
        self.selected = bool(selected)


class TargetTracker:
    """交互式单目标跟踪器。

    先用 ``select(x1, y1, x2, y2)`` 传入屏幕坐标的框选区域，再把检测
    结果的 ``boxes`` 持续传给 ``update``。首次匹配到框选区域的检测框后，
    该目标的 ByteTrack ID 会被锁定，之后只返回这个目标；``clear`` 取消
    锁定。它不需要额外 NPU 模型，和 ObjectTracker 共用检测结果。
    """
    def __init__(self, high_score=0.45, low_score=0.15,
                 iou_threshold=0.30, max_missed=30,
                 select_iou=0.05):
        self._tracker = ObjectTracker(high_score, low_score,
                                      iou_threshold, max_missed)
        self._select_iou = float(select_iou)
        self._selection = None
        self._selected_id = None

    @staticmethod
    def _iou(a, b):
        ix1, iy1 = max(a.x1, b.x1), max(a.y1, b.y1)
        ix2, iy2 = min(a.x2, b.x2), min(a.y2, b.y2)
        iw, ih = max(0, ix2 - ix1), max(0, iy2 - iy1)
        inter = iw * ih
        if inter <= 0:
            return 0.0
        union = a.width * a.height + b.width * b.height - inter
        return inter / union if union > 0 else 0.0

    @staticmethod
    def _box(x1, y1, x2, y2):
        x1, x2 = sorted((float(x1), float(x2)))
        y1, y2 = sorted((float(y1), float(y2)))
        if x2 <= x1 or y2 <= y1:
            raise ValueError("selection box must have positive size")
        return Box(int(x1), int(y1), int(x2), int(y2), 1.0, -1, [])

    def select(self, x1, y1, x2, y2):
        """设置屏幕坐标框选区域；下一帧从该区域选择目标。"""
        self._selection = self._box(x1, y1, x2, y2)
        self._selected_id = None

    def select_box(self, box):
        """用一个已有的 Box 作为框选区域。"""
        self.select(box.x1, box.y1, box.x2, box.y2)

    def clear(self):
        """取消当前目标锁定并清空跟踪器状态。"""
        self._selection = None
        self._selected_id = None
        self._tracker.reset()

    reset = clear

    @property
    def selected_id(self):
        return self._selected_id

    @property
    def selecting(self):
        return self._selection is not None and self._selected_id is None

    def update(self, boxes):
        tracks = self._tracker.update(boxes, include_lost=True)
        if self._selection is not None and self._selected_id is None:
            candidates = [(self._iou(self._selection, t.box), t)
                          for t in tracks if not t.missed]
            if candidates:
                score, track = max(candidates, key=lambda item: item[0])
                # For a loose drag box, center-in-ROI is also a valid match.
                inside = (self._selection.x1 <= track.center_x <= self._selection.x2
                          and self._selection.y1 <= track.center_y <= self._selection.y2)
                if score >= self._select_iou or inside:
                    self._selected_id = track.track_id
        if self._selected_id is None:
            return None
        for track in tracks:
            if track.track_id == self._selected_id:
                return TargetTrack(track)
        return None


class CountEvent:
    """一条轨迹穿越竖直计数线时产生的事件。"""
    __slots__ = ("track_id", "direction", "center_x", "center_y")

    def __init__(self, track_id, direction, center_x, center_y):
        self.track_id = track_id
        self.direction = direction
        self.center_x = center_x
        self.center_y = center_y


class LineCounter:
    """基于稳定 track_id 的双向竖直过线计数器。"""
    def __init__(self, line_x=360, once_per_direction=True):
        self.line_x = float(line_x)
        self.once_per_direction = bool(once_per_direction)
        self.left_to_right = 0
        self.right_to_left = 0
        self._counted = set()

    @property
    def total(self):
        return self.left_to_right + self.right_to_left

    def update(self, tracks):
        events = []
        for track in tracks:
            if track.missed or track.age <= 1:
                continue
            direction = None
            if (track.previous_center_x < self.line_x and
                    track.center_x >= self.line_x):
                direction = "left_to_right"
            elif (track.previous_center_x > self.line_x and
                  track.center_x <= self.line_x):
                direction = "right_to_left"
            if direction is None:
                continue
            key = (track.track_id, direction)
            if self.once_per_direction and key in self._counted:
                continue
            self._counted.add(key)
            if direction == "left_to_right":
                self.left_to_right += 1
            else:
                self.right_to_left += 1
            events.append(CountEvent(track.track_id, direction,
                                     track.center_x, track.center_y))
        return events

    def reset(self):
        self.left_to_right = 0
        self.right_to_left = 0
        self._counted.clear()


def _resolve_spec(spec):
    if os.path.exists(spec):
        return spec
    if not os.path.isabs(spec):
        cand = os.path.join(DEFAULT_MODEL_DIR, spec)
        if os.path.exists(cand):
            return cand
        raise FileNotFoundError(
            "model spec 不存在: %r（也找过 %r）" % (spec, cand))
    raise FileNotFoundError("model spec 不存在: %r" % spec)


def load(spec, threshold=0.5, top_k=5, firmware="",
         face_spec=None, expand=0.2, dense=True, to_screen=True,
         match_threshold=0.6, max_faces=3, max_hands=2, ema_alpha=0.65,
         smooth_frames=5) -> Model:
    """加载 .mud 模型，自动识别族，返回 Model（加载失败立刻抛）。

    - spec: .mud 路径；短名在 DEFAULT_MODEL_DIR 下解析
    - threshold: 检测/OCR 置信度阈值
    - top_k: 分类返回条数
    - face_spec: face_dense 族第一级人脸检测模型，缺省取 spec 同目录
      scrfd_real.mud
    - expand: face_dense 脸框外扩比例（ROI 留白，0.2 = +20%）
    - dense: face_dense 族置 False 时跳过稠密模型，只出 SCRFD 五点
    - to_screen: 默认把结果坐标从推理帧映射到 720x480 屏幕坐标系
      （纯 Python 副本）；False 返回 tdl_py 原始结果（帧坐标系）
    """
    spec = _resolve_spec(spec)
    family = _detect_family(spec)
    extra = _extra(spec)

    if family == "face_dense":
        face_spec = face_spec or os.path.join(os.path.dirname(spec),
                                              "scrfd_real.mud")
        pipeline = _FaceDensePipeline(threshold, face_spec, expand, dense)
    elif family == "face_recognition":
        detector_model = _extra_path(spec, extra.get("detector_model"),
                                     "scrfd_real.mud")
        feature_model = _extra_path(spec, extra.get("feature_model"),
                                    "feature_cviface.mud")
        pipeline = _FaceRecognitionPipeline(detector_model, feature_model,
                                            threshold, match_threshold,
                                            max_faces)
    elif family == "face_emotion":
        detector_model = _extra_path(spec, extra.get("detector_model"),
                                     "scrfd_real.mud")
        attribute_model = _extra_path(
            spec, extra.get("attribute_model"),
            "face_attribute_gender_age_glass_emotion.mud")
        pipeline = _FaceEmotionPipeline(detector_model, attribute_model,
                                        threshold, max_faces)
    elif family == "hand_gesture":
        detector_model = _extra_path(spec, extra.get("detector_model"),
                                     "yolov8n_det_hand.mud")
        keypoint_model = _extra_path(spec, extra.get("keypoint_model"),
                                     "keypoint_hand_128.mud")
        pipeline = _HandGesturePipeline(detector_model, keypoint_model,
                                        threshold, max_hands)
    elif family == "pose_classifier":
        pose_model = _extra_path(spec, extra.get("keypoint_model"),
                                 "pose_yolov8.mud")
        pipeline = _PoseClassifierPipeline(pose_model, threshold, ema_alpha,
                                           smooth_frames)
    elif family == "self_learning":
        pipeline = _SelfLearningPipeline(top_k)
    elif family == "tracking":
        pipeline = _FearTrackPipeline()
    elif family == "classification":
        a = tdl_py.Classifier()
        pipeline = _SinglePipeline(a, lambda f: a.classify(f, 0.0, top_k))
    elif family == "keypoint":
        a = tdl_py.KeypointDetector()
        pipeline = _SinglePipeline(a, lambda f: a.estimate(f))
    elif family == "seg":
        a = tdl_py.InstanceSegmenter()
        pipeline = _SinglePipeline(a, lambda f: a.segment(f))
    elif family == "ocr":
        a = tdl_py.PlateRecognizer()
        pipeline = _SinglePipeline(a, lambda f: a.recognize(f, threshold))
    else:
        a = tdl_py.Detector()
        pipeline = _SinglePipeline(a, lambda f: a.detect(f, threshold))

    pipeline.load(spec, firmware)
    return Model(spec, family, pipeline, to_screen)
