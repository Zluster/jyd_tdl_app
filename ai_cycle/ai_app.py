"""独立 AI 轮换应用逻辑：模型常驻池 + 定时轮换，推理结果实时绘制。

复制改造自 launcher/apps/ai/app.py 与 apps/vision.py 的 LinePool/LabelPool
（按约定不动老代码，本文件自包含，不依赖 appfw/AppContext）。

架构要点——模型常驻池：启动时由 worker 串行加载全部模型并各预热一帧，
轮换只换活跃管线的引用，不再 reset/reload。这么做的实测依据：bmrt 0.4.9
的 load/destroy 路径不干净——每次 load 在主机堆留 ~1MB 残渣（bm-smi
设备内存/句柄/线程均正常，VmData 随轮换单调上涨），反复 ~96 轮后在
load 内段错误。常驻后 load 只发生一次，泄漏与崩溃同时消失，切换也从
秒级 loading 变为瞬时。

其余设计：
- 加载/预热失败的模型剔除出轮换（日志有原因）；全部失败 -> fatal 置位，
  宿主（app.py）退出交给外部看门狗；
- 取帧+推理在 worker 线程（释放 GIL），结果槽发布 (family, result, fps)，
  绘制函数跟结果自带的族走，切换瞬间不错配；
- AiCycler.update(dt_ms) 由宿主 tick 循环每帧驱动：running 态计满
  SWITCH_MS 毫秒切下一个，循环往复。
"""

import configparser
import os
import struct
import threading
import time

import tdl_py

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

# MicroPython 侧线段/文本池（apps/vision.py 的裁剪版：只保留批量接口）。
# 注意：SRC 顶部不能放 _vlines = [] 这类模块级初始化——LinePool 和
# LabelPool 各自 exec 一次本源码，顶层重置会清掉对方已注册的池。
_POOL_SRC = r"""
import lvgl as lv

def _vline_pool_init(scr, n, color_hex, width):
    global _vlines
    _vlines = []
    for i in range(n):
        pts = [{'x': 0, 'y': 0}, {'x': 0, 'y': 0}]
        ln = lv.line(scr)
        ln.set_style_line_color(lv.color_hex(color_hex), 0)
        ln.set_style_line_width(width, 0)
        ln.set_points(pts, 2)
        ln.add_flag(lv.obj.FLAG.HIDDEN)
        _vlines.append((ln, pts))

def _vlines_batch(payload):
    # 批量线段：int16 LE 扁平 [x1,y1,x2,y2]*n，多余的线隐藏（检测框类）
    n = len(payload) // 8
    for i in range(len(_vlines)):
        ln, pts = _vlines[i]
        if i < n:
            b = i * 8
            vals = []
            for k in range(4):
                v = payload[b + k * 2] | (payload[b + k * 2 + 1] << 8)
                if v >= 32768:
                    v -= 65536
                vals.append(v)
            pts[0]['x'] = vals[0]
            pts[0]['y'] = vals[1]
            pts[1]['x'] = vals[2]
            pts[1]['y'] = vals[3]
            ln.set_points(pts, 2)
            ln.remove_flag(lv.obj.FLAG.HIDDEN)
        else:
            ln.add_flag(lv.obj.FLAG.HIDDEN)

def _vlabel_pool_init(scr, n, color_hex):
    global _vlabels
    _vlabels = []
    for i in range(n):
        lb = lv.label(scr)
        lb.set_style_text_color(lv.color_hex(color_hex), 0)
        lb.set_text("")
        lb.add_flag(lv.obj.FLAG.HIDDEN)
        _vlabels.append(lb)

def _vlabel_set(i, x, y, text):
    lb = _vlabels[i]
    lb.set_text(text)
    lb.set_pos(x, y)
    lb.remove_flag(lv.obj.FLAG.HIDDEN)

def _vlabel_hide_all():
    for lb in _vlabels:
        lb.add_flag(lv.obj.FLAG.HIDDEN)
"""


class LinePool:
    """n 条 LVGL 线段的批量更新接口（构造改收 m + screen，摆脱 AppContext）。"""

    def __init__(self, m, screen, n, color=0x00E676, width=3):
        m.exec(_POOL_SRC)
        # screen 是代理对象，init 走 proxy_call 编组；后续全标量走 m.call 快速路径
        m.proxy_call("_vline_pool_init", screen, n, color, width)
        self._m = m

    def set_batch(self, payload: bytes):
        """批量线段：int16 LE 扁平 [x1,y1,x2,y2]*n，传 b"" 隐藏全部。"""
        self._m.call("_vlines_batch", payload)


class LabelPool:
    """n 个 LVGL label 的池：检测框标签/分类列表这类小文本。"""

    def __init__(self, m, screen, n, color=0xFFFFFF):
        m.exec(_POOL_SRC)
        m.proxy_call("_vlabel_pool_init", screen, n, color)
        self._m = m
        self._n = n

    def set_all(self, items):
        """items: [(x, y, text), ...]，先全隐再逐个设置。"""
        m = self._m
        m.call("_vlabel_hide_all")
        for i, (x, y, text) in enumerate(items[:self._n]):
            m.call("_vlabel_set", i, int(x), int(y), str(text))


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
    映射 = 各自去黑边后按内容归一（本组数值下退化为纯平移）。"""
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


class AiCycler:
    """模型常驻池 + 轮换调度 + 推理 worker + 结果绘制。

    宿主每 tick 调 update(dt_ms)；start() 后 worker 先 _preload_all()
    （串行加载全部模型并各预热一帧），再进入取帧推理循环。"""

    SWITCH_MS = 10_000       # 每个模型的展示时长
    STATUS_TICKS = 15        # 状态栏刷新节流（30Hz tick 下约 0.5s 一次）

    def __init__(self, m, lv, screen, models, threshold=0.5, top_k=5,
                 firmware="", expand=0.2):
        assert models, "models 不能为空"
        self._m = m
        self._lv = lv
        self._models = models          # [(spec_path, {"dense": bool}), ...]
        self._threshold = threshold
        self._top_k = top_k
        self._firmware = firmware
        self._expand = expand   # 脸框外扩比例（ROI 留白，0.2 = +20%）

        lv.layer_bottom().set_style_bg_opa(0, lv.STATE.DEFAULT)
        screen.set_style_bg_opa(lv.OPA.TRANSP, 0)

        self._status = lv.label(screen)
        self._status.set_style_text_color(lv.color_white(), 0)
        self._status.set_text("starting...")
        self._status.align(lv.ALIGN.TOP_MID, 0, 8)

        self._lines = LinePool(m, screen, _MAX_LINES)
        self._labels = LabelPool(m, screen, _MAX_LABELS)

        # 常驻池与状态机：preload -> running -(SWITCH_MS)-> 下一个（只换引用）
        self._pool = []                # [(name, pipeline, family)]，预热成功的模型
        self._active = 0               # 池下标（worker 写，UI 只读）
        self._state = "preload"
        self._preload_note = "waiting..."
        self._family = ""
        self._run_ms = 0
        self._fps = 0.0
        self._tick_n = 0
        self._last_status = ""
        self._draw_err_n = 0
        #: 全部模型加载失败时置位，宿主（app.py）应退出交给看门狗
        self.fatal = False

        # worker 共享槽（互斥区只做引用交换，不做耗时操作）
        self._map = None               # 首帧按实际尺寸建 letterbox 映射（预热时建）
        self._req = None               # int：目标池下标，UI 挂请求，worker 消费
        self._req_lock = threading.Lock()
        self._event = None             # ("preloading", i, n, name)
                                       # ("preload_fail", i, n, name)
                                       # ("loaded", idx, family) / ("fatal", msg)
        self._latest = None            # (family, result, fps)：worker 覆盖写
        self._out_lock = threading.Lock()
        self._worker_err_n = 0
        self._stop = threading.Event()
        self._worker = None
        self._cam = None

    # ---- 生命周期 ----

    def start(self):
        # timeout_ms=200：约束退出时 worker 的 join 耗时（read 最长阻塞 200ms）
        self._cam = tdl_py.VpssCamera.ai(timeout_ms=200)
        self._cam.open()
        self._worker = threading.Thread(target=self._infer_loop, daemon=True)
        self._worker.start()

    def close(self):
        # 先停 worker 再释放资源：worker 可能在 read/推理中
        self._stop.set()
        if self._worker is not None:
            self._worker.join(timeout=2.0)
            self._worker = None
        for _name, pipeline, _family in self._pool:
            try:
                pipeline.reset()
            except Exception:
                pass
        self._pool = []
        if self._cam is not None:
            try:
                self._cam.close()
            except Exception:
                pass
            self._cam = None

    # ---- UI 线程：宿主 tick 循环每帧调用 ----

    def update(self, dt_ms: int):
        with self._out_lock:
            ev, self._event = self._event, None
            latest, self._latest = self._latest, None

        if ev is not None:
            kind = ev[0]
            if kind == "preloading":
                _, i, n, name = ev
                self._preload_note = "loading %d/%d: %s ..." % (i + 1, n, name)
            elif kind == "preload_fail":
                _, i, n, name = ev
                self._preload_note = "%d/%d: %s FAILED, skipped" % (i + 1, n, name)
            elif kind == "loaded":
                _, idx, family = ev
                self._state = "running"
                self._family = family
                self._run_ms = 0
                self._fps = 0.0
            elif kind == "fatal":
                self._preload_note = "FATAL: " + ev[1]
                self.fatal = True
            self._refresh_status(force=True)

        if self._state == "running":
            if latest is not None:
                family, result, fps = latest
                self._fps = fps
                try:
                    getattr(self, "_draw_" + family)(result)
                except Exception as e:
                    self._draw_err_n += 1
                    if self._draw_err_n % 25 == 1:   # 节流打印，防刷日志
                        print("ai_cycle: draw error (%d): %s"
                              % (self._draw_err_n, e))
            self._run_ms += dt_ms
            if self._run_ms >= self.SWITCH_MS:
                self._request((self._active + 1) % len(self._pool))
        # preload 态：只等事件，不推理不计时

        self._tick_n += 1
        if self._tick_n % self.STATUS_TICKS == 0:
            self._refresh_status()

    # ---- 轮换调度 ----

    def _request(self, idx):
        """UI 线程发起切换：清屏 + 挂请求；常驻池无需加载，下一帧即生效。"""
        self._lines.set_batch(b"")      # 旧框/骨架立即清掉，不残留到下个模型
        self._labels.set_all([])
        with self._req_lock:
            self._req = idx

    def _refresh_status(self, force=False):
        if self._state != "running":
            text = "[preload] " + self._preload_note
        else:
            name = self._pool[self._active][0]
            remain = max(0, (self.SWITCH_MS - self._run_ms + 999) // 1000)
            text = "[%d/%d] %s (%s)  %.1f fps  next: %ds" % (
                self._active + 1, len(self._pool), name,
                self._family, self._fps, remain)
        if force or text != self._last_status:
            self._status.set_text(text)
            self._last_status = text

    # ---- worker 线程：换模型 + 取帧 + 推理 ----

    def _make_algo(self, family, spec, dense):
        """按族建管线（复制自 AiApp._create_algo，face_spec 按 spec 目录推导）。"""
        if family == "face_dense":
            face_spec = os.path.join(os.path.dirname(spec), "scrfd_real.mud")
            return _FaceDensePipeline(self._threshold, face_spec,
                                      self._expand, dense)
        if family == "classification":
            a = tdl_py.Classifier()
            return _SinglePipeline(a, lambda f: a.classify(f, 0.0, self._top_k))
        if family == "keypoint":
            a = tdl_py.KeypointDetector()
            return _SinglePipeline(a, lambda f: a.estimate(f))
        if family == "seg":
            a = tdl_py.InstanceSegmenter()
            return _SinglePipeline(a, lambda f: a.segment(f))
        if family == "ocr":
            a = tdl_py.PlateRecognizer()
            return _SinglePipeline(a, lambda f: a.recognize(f, self._threshold))
        a = tdl_py.Detector()
        return _SinglePipeline(a, lambda f: a.detect(f, self._threshold))

    def _preload_all(self):
        """worker 启动阶段：串行加载全部模型，每个各预热一帧。

        预热的目的：各模型的 VPSS 预处理组在首次推理时才创建，启动期
        全部建出来——组数耗尽/推理异常等问题立刻暴露（该模型被剔除），
        而不是轮换到才炸。预热读帧允许重试（相机刚开可能无帧），
        load 失败则直接剔除。返回 False 表示全部被剔除或收到停止。"""
        n = len(self._models)
        for i, (spec, kw) in enumerate(self._models):
            if self._stop.is_set():
                return False
            name = os.path.splitext(os.path.basename(spec))[0]
            with self._out_lock:
                self._event = ("preloading", i, n, name)
            pipeline = None
            try:
                family = _detect_family(spec)
                pipeline = self._make_algo(family, spec, kw.get("dense", True))
                pipeline.load(spec, self._firmware)
            except Exception as e:
                print("ai_cycle: preload '%s' load failed: %s" % (name, e))
                pipeline = None
            warmed = False
            while pipeline is not None and not warmed:
                for attempt in range(5):
                    if self._stop.is_set():
                        return False
                    try:
                        with self._cam.read() as frame:
                            if self._map is None:
                                self._map = _make_ai_map(frame.width,
                                                         frame.height)
                            pipeline.run(frame)   # 预热：建 VPSS 组 + 端到端验证
                        warmed = True
                        break
                    except Exception as e:
                        print("ai_cycle: preload '%s' warmup (%d/5) failed: %s"
                              % (name, attempt + 1, e))
                        time.sleep(0.2)
                if not warmed:
                    try:
                        pipeline.reset()
                    except Exception:
                        pass
                    pipeline = None
            if pipeline is None:
                with self._out_lock:
                    self._event = ("preload_fail", i, n, name)
                continue
            self._pool.append((name, pipeline, family))
        if not self._pool:
            with self._out_lock:
                self._event = ("fatal", "all models failed to load")
            return False
        return True

    def _infer_loop(self):
        """worker：常驻池加载 -> 阻塞取帧 + 推理（都释放 GIL）。
        切换只换 _active 引用，结果发布到 _latest 槽。"""
        if not self._preload_all():
            return
        fps = 0.0
        with self._out_lock:   # 首发模型直接开跑
            self._event = ("loaded", 0, self._pool[0][2])
        while not self._stop.is_set():
            with self._req_lock:
                req, self._req = self._req, None
            if req is not None:
                self._active = req
                fps = 0.0
                with self._out_lock:
                    self._event = ("loaded", req, self._pool[req][2])
                    self._latest = None      # 丢弃上个模型的残留结果

            pipeline = self._pool[self._active][1]
            t0 = time.time()
            try:
                with self._cam.read() as frame:
                    result = pipeline.run(frame)   # 推理必须在 with 内（frame 出块失效）
            except Exception as e:
                self._worker_err_n += 1
                if self._worker_err_n % 25 == 1:   # 节流打印，防刷日志
                    print("ai_cycle worker error (%d): %s"
                          % (self._worker_err_n, e))
                time.sleep(0.05)                   # 防异常空转
                continue
            dt = time.time() - t0
            if dt > 0:
                inst = 1.0 / dt
                fps = inst if fps <= 0 else fps * 0.8 + inst * 0.2
            with self._out_lock:
                self._latest = (self._pool[self._active][2], result, fps)

    # ---- 结果渲染：坐标映射 + 批量下发（复制自 AiApp） ----

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
                for p in pts:   # 五点：十字，稀疏醒目
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