"""jyd.nn：.mud 模型统一加载与推理。

nn.load(spec) 自动识别模型族并返回带 run(frame) 的 Model：

    detection        目标检测        run -> Result（.boxes / .label_of）
    classification   分类            run -> Result（.classes）
    keypoint         关键点/姿态     run -> KeypointResult（.points）
    seg              实例分割        run -> SegResult（.instances）
    ocr              车牌/OCR        run -> Result（.boxes / .attributes / .text）
    face_dense       人脸稠密关键点  run -> [(Box, [Point]), ...]（两级管线）

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
DEFAULT_MODEL_DIR = "/root/tdl_app_sdk_cv184x/configs/model_specs/"

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
        return _convert(self.family, raw, m)

    def reset(self):
        """显式卸载模型（NPU 资源）。进程退出时操作系统会兜底回收。"""
        self._pipeline.reset()

    def __repr__(self):
        return "<jyd.nn.Model %s (%s)>" % (os.path.basename(self.spec),
                                           self.family)


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
         face_spec=None, expand=0.2, dense=True, to_screen=True) -> Model:
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

    if family == "face_dense":
        face_spec = face_spec or os.path.join(os.path.dirname(spec),
                                              "scrfd_real.mud")
        pipeline = _FaceDensePipeline(threshold, face_spec, expand, dense)
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
