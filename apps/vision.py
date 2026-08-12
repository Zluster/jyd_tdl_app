"""视觉应用公共件。

- VisionApp：同步取帧 + NV12 Y plane 零拷贝灰度图的模板基类，
  子类只需实现 process(img) 跑各自的识别；
- LinePool：LVGL 线段池。list[dict] 无法跨 mpyc 桥编组（marshal_arg
  只支持标量/bytes/callable/代理），所以 lv.line 和点数组建在
  MicroPython 侧，CPython 每帧只传标量坐标。
"""

import threading
import time

import _maix_image as image

from appfw import App, AppContext

#: VisionWorkerApp 结果槽的空标记（区别于合法的 None 结果——
#: "识别不到目标"也是结果，draw(None) 要负责清屏）
_NO_NEW_RESULT = object()

# MicroPython 侧线段池实现。重复 exec 只是重定义同名函数，幂等；
# 池本身在 _vline_pool_init 里重置，与应用 screen 同生命周期
# （screen 删除时线段控件一并回收，框架退出应用时 gc.collect 兜底）
# 注意：SRC 顶部不能放 _vlines = [] / _vlabels = [] 这类模块级初始化——
# LinePool 和 LabelPool 各自 exec 一次本源码，顶层的重置会清掉对方
# 已注册的池（AiApp 框不显示即此 bug）。全局名一律由 *_pool_init 创建。
_LINE_POOL_SRC = r"""
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

def _vline_set(i, x1, y1, x2, y2):
    ln, pts = _vlines[i]
    pts[0]['x'] = x1
    pts[0]['y'] = y1
    pts[1]['x'] = x2
    pts[1]['y'] = y2
    ln.set_points(pts, 2)
    ln.remove_flag(lv.obj.FLAG.HIDDEN)

def _vline_hide(i):
    _vlines[i][0].add_flag(lv.obj.FLAG.HIDDEN)

def _vbars_set(x0, step, base, heights):
    # 批量竖条：一次桥调用更新全部，heights 是 int16 LE bytes
    n = len(_vlines)
    m = len(heights) // 2
    if m < n:
        n = m
    for i in range(n):
        h = heights[i * 2] | (heights[i * 2 + 1] << 8)
        ln, pts = _vlines[i]
        x = x0 + i * step
        pts[0]['x'] = x
        pts[0]['y'] = base
        pts[1]['x'] = x
        pts[1]['y'] = base - h
        ln.set_points(pts, 2)
        ln.remove_flag(lv.obj.FLAG.HIDDEN)

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
    """n 条 LVGL 线段的标量更新接口。width 是线宽（像素），频谱竖条用粗线。"""

    def __init__(self, ctx: AppContext, n: int, color: int = 0x00E676, width: int = 3):
        m = ctx.m
        m.exec(_LINE_POOL_SRC)
        # screen 是代理对象，init 走 proxy_call 编组；后续全标量走 m.call 快速路径
        m.proxy_call("_vline_pool_init", ctx.screen, n, color, width)
        self._m = m

    def set(self, i: int, x1: int, y1: int, x2: int, y2: int):
        self._m.call("_vline_set", i, int(x1), int(y1), int(x2), int(y2))

    def hide(self, i: int):
        self._m.call("_vline_hide", i)

    def set_bars(self, x0: int, step: int, base: int, heights: bytes):
        """批量竖条（频谱类）：一根 bytes 调用更新全部，替代 n 次 set。

        heights 是 int16 LE 的 bytes（np.asarray(h, dtype='<i2').tobytes()），
        第 i 根条画 (x0+i*step, base) -> (x0+i*step, base-h[i])。
        逐根 set 每帧要 n 次桥调用，实测把 25Hz tick 拖到 5FPS。"""
        self._m.call("_vbars_set", x0, step, base, heights)

    def set_batch(self, payload: bytes):
        """批量线段（检测框/骨架/多边形）：int16 LE 扁平 [x1,y1,x2,y2]*n。

        一次桥调用更新全部线段，池中多余线段自动隐藏；
        传 b"" 隐藏全部。"""
        self._m.call("_vlines_batch", payload)


class LabelPool:
    """n 个 LVGL label 的池：检测框标签/分类列表这类小文本。"""

    def __init__(self, ctx: AppContext, n: int, color: int = 0xFFFFFF):
        m = ctx.m
        m.exec(_LINE_POOL_SRC)
        m.proxy_call("_vlabel_pool_init", ctx.screen, n, color)
        self._m = m
        self._n = n

    def set_all(self, items):
        """items: [(x, y, text), ...]，先全隐再逐个设置。"""
        m = self._m
        m.call("_vlabel_hide_all")
        for i, (x, y, text) in enumerate(items[:self._n]):
            m.call("_vlabel_set", i, int(x), int(y), str(text))


class VisionApp(App):
    """同步取帧 + 零拷贝灰度图的模板基类。子类实现 process(img)。"""

    def _vision_init(self, ctx: AppContext):
        self._ctx = ctx
        self._lv = ctx.lv
        self._stride_warned = False

    def on_update(self, dt_ms: int):
        # 同步取帧会阻塞到下一帧到达：UI tick 节奏与摄像头帧率绑定。
        # img 引用帧内存，出 with 块 frame 即失效——process 必须当帧跑完，
        # 识别结果（坐标/文本）是拷贝出的标量，出块后随便用
        with self._ctx.camera.read() as frame:
            if frame.strides[0] != frame.width:
                # image.new 按紧密排列解释内存，stride 有 padding 时零拷贝不成立
                if not self._stride_warned:
                    print("%s: stride %d != width %d, zero-copy gray view invalid"
                          % (self.name, frame.strides[0], frame.width))
                    self._stride_warned = True
                return
            img = image.new(size=(frame.width, frame.height), mode="L",
                            addr=frame.addr)
            self.process(img)

    def process(self, img):
        raise NotImplementedError


class VisionWorkerApp(App):
    """worker 线程取帧 + 识别的模板基类（重识别应用用）。

    与 VisionApp 的区别：find_qrcodes/find_barcodes 这类 CPU CV 一跑就是
    几十~几百 ms，即使取帧不阻塞（VisionApp 的 5ms 轮询），识别本身仍会
    把 UI tick 拖死、触摸/退出失灵。这里取帧+识别放 worker 线程（read
    释放 GIL），on_update 只取最新结果绘制，UI 循环恒不被拖慢。

    子类约定（从 VisionApp 迁过来只改这三处）：
    - 基类换成 VisionWorkerApp；
    - process(img) -> result：worker 内执行，只准 maix/数值计算，
      禁碰 LVGL/桥调用（LVGL 非线程安全）；结果必须是拷贝出的标量；
    - draw(result)：UI 线程执行，原来的 LVGL 更新逻辑搬这里。
    """

    def _vision_init(self, ctx: AppContext):
        self._ctx = ctx
        self._lv = ctx.lv
        self._stride_warned = False
        # 最新结果槽：worker 覆盖写，on_update 取走清空
        self._latest = _NO_NEW_RESULT
        self._latest_lock = threading.Lock()
        self._worker_stop = threading.Event()
        self._err_n = 0
        self._worker = threading.Thread(target=self._worker_loop, daemon=True)
        self._worker.start()

    def _worker_loop(self):
        while not self._worker_stop.is_set():
            try:
                with self._ctx.camera.read() as frame:
                    if frame.strides[0] != frame.width:
                        # image.new 按紧密排列解释内存，stride 有 padding 时零拷贝不成立
                        if not self._stride_warned:
                            print("%s: stride %d != width %d, zero-copy gray view invalid"
                                  % (self.name, frame.strides[0], frame.width))
                            self._stride_warned = True
                        continue
                    img = image.new(size=(frame.width, frame.height), mode="L",
                                    addr=frame.addr)
                    result = self.process(img)   # 必须在 with 内（frame 出块失效）
            except Exception as e:
                if "failed to get frame" in str(e):
                    time.sleep(0.01)   # 相机是 5ms 轮询：补个节拍防热 spin
                    continue
                self._err_n += 1
                if self._err_n % 25 == 1:   # 节流打印，防刷日志
                    print("%s worker error (%d): %s" % (self.name, self._err_n, e))
                time.sleep(0.05)            # 防异常空转
                continue
            with self._latest_lock:
                self._latest = result

    def on_update(self, dt_ms: int):
        with self._latest_lock:
            latest = self._latest
            self._latest = _NO_NEW_RESULT
        if latest is not _NO_NEW_RESULT:
            self.draw(latest)

    def on_destroy(self):
        # 先停 worker 再由框架回收其余资源：worker 可能在 read/识别中
        self._worker_stop.set()
        worker = getattr(self, "_worker", None)
        if worker is not None:
            worker.join(timeout=2.0)
            self._worker = None

    def process(self, img):
        raise NotImplementedError

    def draw(self, result):
        raise NotImplementedError
