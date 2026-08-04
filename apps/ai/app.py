"""AI 模型检测应用：传入 .mud model-spec，6 族自适应。

仿 sophpi_ai_osd_demo.cpp：
- ai 通道（grp0/ch1，640x640 letterbox）自开相机取帧推理（NPU，GIL 释放）；
- 取帧+推理在 worker 线程（read/推理都释放 GIL，UI tick 不被推理阻塞），
  on_update 只取 worker 最新结果绘制；结果为拷贝出的标量，天然可跨线程；
- 结果坐标经 letterbox 映射回 720x480 屏幕（照抄 C++ CoordMap）；
- 框/骨架/多边形走 LinePool 批量，文本走 LabelPool；
- 族判定照抄 C++ detectFamily（.mud [extra] 的 task/model_type）；
  另有 face_dense 族（face_dense_real.mud）：两级管线——Detector
  （scrfd_real.mud，缺省取 spec 同目录，face_spec 可覆盖）出脸框，
  FaceDenseLandmark 逐脸估计稠密关键点。

模型加载/相机打开失败 -> on_create 抛异常 -> 框架回菜单（看串口日志）。
"""

import configparser
import os
import struct
import threading
import time

import tdl_py

from appfw import App, AppContext
from apps.vision import LinePool, LabelPool

_LIVE_W, _LIVE_H = 720, 480
_SRC_W, _SRC_H = 1600, 1200   # sensor 原生尺寸：ai 帧和屏幕都是它的
                              # 等比缩放+黑边（4:3），坐标映射以它为源
_MAX_LINES = 80
_MAX_LABELS = 6
_MAX_BOXES = 6

# 17 点 COCO 姿态骨架（照抄 C++ kPose17Skeleton）
_POSE17_EDGES = ((0, 1), (0, 2), (1, 3), (2, 4), (5, 6), (5, 7), (7, 9),
                 (6, 8), (8, 10), (5, 11), (6, 12), (11, 12), (11, 13),
                 (13, 15), (12, 14), (14, 16))


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


def _letterbox(frame_w, frame_h, src_w, src_h):
    """src 等比嵌入 frame_w×frame_h 加黑边：返回 (ox, oy, 内容宽, 内容高)。"""
    fit = min(frame_w / src_w, frame_h / src_h)
    cw, ch = src_w * fit, src_h * fit
    return (frame_w - cw) / 2.0, (frame_h - ch) / 2.0, cw, ch


def _make_ai_map(frame_w, frame_h):
    """ai 帧与屏幕显示的都是同一 1600x1200 sensor 画面的等比嵌入：
    ai 640x640 上下黑边（内容 640x480, oy=80），屏幕 720x480 左右黑边
    （内容 640x480, ox=40）。映射 = 各自去黑边后按内容归一：
    本组数值下退化为 x' = x + 40, y' = y - 80（纯平移）。"""
    ox_a, oy_a, cw_a, ch_a = _letterbox(frame_w, frame_h, _SRC_W, _SRC_H)
    ox_s, oy_s, cw_s, ch_s = _letterbox(_LIVE_W, _LIVE_H, _SRC_W, _SRC_H)

    def m(x, y):
        return (int((x - ox_a) / cw_a * cw_s + ox_s),
                int((y - oy_a) / ch_a * ch_s + oy_s))
    return m


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

    dense=False 时跳过第二级（不加载稠密模型），只画 SCRFD 自带的
    五点（box.landmarks：双眼/鼻尖/嘴角）——用于隔离验证坐标链路。"""

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
        for box in r.boxes[:2]:   # 稠密点线多，最多跟 2 张脸
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


class AiApp(App):
    name = "ai"

    def __init__(self, model_spec, threshold=0.5, top_k=5, firmware="",
                 face_spec=None, expand=0.2, dense=True):
        self._spec = model_spec
        self._threshold = threshold
        self._top_k = top_k
        self._firmware = firmware
        # face_dense 族的第一级人脸检测模型，缺省取 spec 同目录 scrfd_real.mud
        self._face_spec = face_spec or os.path.join(
            os.path.dirname(model_spec), "scrfd_real.mud")
        self._expand = expand   # 脸框外扩比例（ROI 留白，0.2 = +20%）
        # dense=False：只画 SCRFD 五点（调试用），不加载稠密模型
        self._dense = dense

    def on_create(self, ctx: AppContext):
        self._ctx = ctx
        self._lv = ctx.lv
        lv, scr = ctx.lv, ctx.screen
        lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
        scr.set_style_bg_opa(lv.OPA.TRANSP, 0)

        self._status = lv.label(scr)
        self._status.set_text("loading model...")
        self._status.set_style_text_color(lv.color_white(), 0)
        self._status.align(lv.ALIGN.TOP_MID, 0, 8)

        self._lines = LinePool(ctx, _MAX_LINES)
        self._labels = LabelPool(ctx, _MAX_LABELS)

        self._family = _detect_family(self._spec)
        self._algo = self._create_algo()
        self._algo.load(self._spec, self._firmware)   # 抛异常 -> 回菜单

        # timeout_ms=200：约束退出时 worker 的 join 耗时（read 最长阻塞
        # 200ms）；正常取帧有帧即返回，不受影响
        self._cam = tdl_py.VpssCamera.ai(timeout_ms=200)
        self._cam.open()
        self._map = None          # 首帧按实际尺寸建 letterbox 映射（worker 内建）
        self._n = 0
        # 最新结果槽：(result, fps)。worker 覆盖写，on_update 取走清空
        self._latest = None
        self._latest_lock = threading.Lock()
        self._worker_stop = threading.Event()
        self._err_n = 0
        self._worker = threading.Thread(target=self._infer_loop, daemon=True)
        self._worker.start()
        self._status.set_text("%s (%s)"
                              % (os.path.basename(self._spec), self._family))

    def _create_algo(self):
        fam = self._family
        if fam == "face_dense":
            return _FaceDensePipeline(self._threshold, self._face_spec,
                                      self._expand, self._dense)
        if fam == "classification":
            a = tdl_py.Classifier()
            return _SinglePipeline(a, lambda f: a.classify(f, 0.0, self._top_k))
        if fam == "keypoint":
            a = tdl_py.KeypointDetector()
            return _SinglePipeline(a, lambda f: a.estimate(f))
        if fam == "seg":
            a = tdl_py.InstanceSegmenter()
            return _SinglePipeline(a, lambda f: a.segment(f))
        if fam == "ocr":
            a = tdl_py.PlateRecognizer()
            return _SinglePipeline(a, lambda f: a.recognize(f, self._threshold))
        a = tdl_py.Detector()
        return _SinglePipeline(a, lambda f: a.detect(f, self._threshold))

    def _infer_loop(self):
        """worker：阻塞取帧 + 推理（都释放 GIL），结果发布到 _latest 槽。
        速率天然与相机/推理能力一致，比 UI 慢时中间帧自动丢弃。"""
        fps = 0.0
        while not self._worker_stop.is_set():
            t0 = time.time()
            try:
                with self._cam.read() as frame:
                    if self._map is None:
                        self._map = _make_ai_map(frame.width, frame.height)
                    result = self._algo.run(frame)   # 推理必须在 with 内（frame 出块失效）
            except Exception as e:
                self._err_n += 1
                if self._err_n % 25 == 1:   # 节流打印，防刷日志
                    print("ai worker error (%d): %s" % (self._err_n, e))
                time.sleep(0.05)            # 防异常空转
                continue
            dt = time.time() - t0
            if dt > 0:
                inst = 1.0 / dt
                fps = inst if fps <= 0 else fps * 0.8 + inst * 0.2
            with self._latest_lock:
                self._latest = (result, fps)

    def on_update(self, dt_ms: int):
        with self._latest_lock:
            latest = self._latest
            self._latest = None
        if latest is None:
            return
        result, fps = latest
        getattr(self, "_draw_" + self._family)(result)

        self._n += 1
        if self._n % 10 == 0:   # 状态栏节流：10 帧刷一次，省桥流量
            self._status.set_text("%s (%s) %.1f fps" % (
                os.path.basename(self._spec), self._family, fps))

    # ---- 结果渲染：坐标映射 + 批量下发 ----

    def _flush(self, coords, label_items):
        self._lines.set_batch(
            struct.pack("<%dh" % len(coords), *coords) if coords else b"")
        self._labels.set_all(label_items)

    def _box_edges(self, x1, y1, x2, y2):
        if x1 > x2:
            x1, x2 = x2, x1
        if y1 > y2:
            y1, y2 = y2, y1
        return [x1, y1, x2, y1, x2, y1, x2, y2,
                x2, y2, x1, y2, x1, y2, x1, y1]

    def _draw_detection(self, r):
        coords, items = [], []
        for box in r.boxes[:_MAX_BOXES]:
            x1, y1 = self._map(box.x1, box.y1)
            x2, y2 = self._map(box.x2, box.y2)
            coords += self._box_edges(x1, y1, x2, y2)
            items.append((min(x1, x2), max(0, min(y1, y2) - 18),
                          "%s %.2f" % (r.label_of(box.class_id), box.score)))
        self._flush(coords, items)

    def _draw_classification(self, r):
        items = []
        for i, c in enumerate(r.classes[:self._top_k]):
            items.append((24, 60 + i * 24,
                          "%d. %s %.1f%%" % (i + 1, r.label_of(c.class_id),
                                             c.score * 100)))
        self._flush([], items)

    def _draw_keypoint(self, r):
        pts = r.points
        coords = []
        if len(pts) == 17:
            for a, b in _POSE17_EDGES:
                pa, pb = pts[a], pts[b]
                if pa.score > 0.05 and pb.score > 0.05:
                    x1, y1 = self._map(pa.x, pa.y)
                    x2, y2 = self._map(pb.x, pb.y)
                    coords += [x1, y1, x2, y2]
        for p in pts:   # 关键点画小十字
            if p.score > 0.3:
                x, y = self._map(p.x, p.y)
                coords += [x - 4, y, x + 4, y, x, y - 4, x, y + 4]
        self._flush(coords[:_MAX_LINES * 4], [])

    def _draw_seg(self, r):
        coords, items = [], []
        for inst in r.instances[:3]:
            box = inst.box
            x1, y1 = self._map(box.x1, box.y1)
            x2, y2 = self._map(box.x2, box.y2)
            coords += self._box_edges(x1, y1, x2, y2)
            items.append((min(x1, x2), max(0, min(y1, y2) - 18),
                          "%d %.2f" % (box.class_id, box.score)))
            out = inst.outline
            if out:
                step = max(1, len(out) // 24)   # 轮廓抽稀，控制线段数
                poly = [self._map(p.x, p.y) for p in out[::step]]
                for i in range(len(poly)):
                    if len(coords) >= _MAX_LINES * 4:
                        break
                    x3, y3 = poly[i]
                    x4, y4 = poly[(i + 1) % len(poly)]
                    coords += [x3, y3, x4, y4]
        self._flush(coords[:_MAX_LINES * 4], items)

    def _draw_face_dense(self, faces):
        coords, items = [], []
        for box, pts in faces:
            x1, y1 = self._map(box.x1, box.y1)
            x2, y2 = self._map(box.x2, box.y2)
            coords += self._box_edges(x1, y1, x2, y2)
            items.append((min(x1, x2), max(0, min(y1, y2) - 18),
                          "face %.2f" % box.score))
            if len(pts) <= 8:
                for p in pts:   # 五点：6px 十字，稀疏醒目
                    x, y = self._map(p.x, p.y)
                    coords += [x - 4, y, x + 4, y, x, y - 4, x, y + 4]
            else:
                for p in pts[:80]:   # 稠密点：2px 短横线，控制线段总数
                    x, y = self._map(p.x, p.y)
                    coords += [x - 1, y, x + 1, y]
                if len(coords) >= _MAX_LINES * 4:
                    break
            if len(coords) >= _MAX_LINES * 4:
                break
        self._flush(coords[:_MAX_LINES * 4], items)

    def _draw_ocr(self, r):
        coords, items = [], []
        prefix = "ocr_text:"
        for i, box in enumerate(r.boxes[:_MAX_BOXES]):
            x1, y1 = self._map(box.x1, box.y1)
            x2, y2 = self._map(box.x2, box.y2)
            coords += self._box_edges(x1, y1, x2, y2)
            text = ""
            if i < len(r.attributes) and r.attributes[i].name.startswith(prefix):
                text = r.attributes[i].name[len(prefix):]
            items.append((min(x1, x2), max(0, min(y1, y2) - 18),
                          text or "%.2f" % box.score))
        if not r.boxes and r.text:
            items.append((24, 90, r.text))
        self._flush(coords, items)

    def on_destroy(self):
        # 先停 worker 再释放资源：worker 可能在 read/推理中
        # （最长 ~timeout_ms + 一次推理），join 等它自然退出
        self._worker_stop.set()
        worker = getattr(self, "_worker", None)
        if worker is not None:
            worker.join(timeout=2.0)
            self._worker = None
        # UI 由框架删 screen 兜底；模型和相机是应用级资源，显式释放
        for res in (getattr(self, "_algo", None), getattr(self, "_cam", None)):
            if res is None:
                continue
            try:
                res.reset() if hasattr(res, "reset") else res.close()
            except Exception:
                pass
