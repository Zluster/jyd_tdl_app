"""jyd.lv：嵌入 LVGL 的模块级代理。

lv.* 全部属性转发到 MicroPython 里的 lvgl 模块（首次访问自动初始化
显示通路：VPSS->VO 链路、OSD 双缓冲、嵌入解释器、触摸驱动）。UI 由
jyd-ui 线程自转（tick + 渲染 + 触摸分发，100 fps 上限），主线程无需
驱动心跳；lv 调用可在任意线程发起——代理内部把每次调用排队转交
jyd-ui 线程执行并等结果（MicroPython 不可重入且绑定该线程）。另有
两个宿主侧扩展函数：

    show(...)        可选的节奏控制 + 图像显示，三种用法：
                       show(fps)        纯 UI 循环按 fps sleep 防空转
                       show(img)        显示/刷新 Image（控件托管见下）
                       show(img, fps)   两者兼有
                     传 img 时模块内部只维护一个 lv.image 控件并复用：
                     同一块像素缓冲只 invalidate() 标脏重绘，缓冲地址/
                     尺寸/格式变了才重设 dsc；控件挂在当前活动屏上，
                     换屏或所在屏被删（appfw 退出应用会删屏）时在新的
                     活动屏上重建；Image 引用由模块保活
    bind(obj, code, fn)   LVGL 事件 -> CPython 无参回调
                          （事件对象不能跨桥，MicroPython 侧吞掉）
    bind_touch(obj)       在对象上采集按下、移动和松开的触摸事件
    read_touch()          读取一条 (phase, x, y) 触摸事件
    LinePool(parent, n)   n 条 lv.line 的批量绘制：draw(lines/boxes/points)
                          一次桥调用画完检测框、骨架、十字点；set_bars 画柱状
    LabelPool(parent, n)  n 个 lv.label 的批量文本：set_all([(x, y, text)])
                          （lv.line 的点数组是 list[dict]，过不了桥，所以控件
                          和点数组都建在 MicroPython 侧，CPython 每帧只传一根
                          bytes；控件随 parent 删除，池自动失效）

另外代理对象的 set_src() 做了增强：可直接传 jyd.image 的 Image
（零拷贝建 dsc；Image 像素内存必须比控件活得久），其余参数照旧转发。

用法：

    from jyd import image, lv

    scr = lv.screen_active()
    btn = lv.button(scr)
    lv.bind(btn, lv.EVENT.CLICKED, lambda: print("clicked"))

    logo = image.load("/root/logo.jpg")   # Image 须保活：像素被控件借用
    w = lv.image(scr)
    w.set_src(logo)                        # 直接传 Image

    while True:
        lv.show(camera.read_image())       # 预览：read 阻塞起搏，无需限速

注意：默认 screen 背景透明（露出摄像头画面），要不透明底自己
set_style_bg_opa。bind 等回调在 jyd-ui 线程的 tick 栈里执行：回调里
可以继续调 lv（同线程直通不排队），与你自己线程共享的状态要自己
留意并发。
"""

import struct

from . import _runtime
from ._mpyc.proxy import Attribute as _Attribute

_BIND_SRC = r"""
import mpy_embed as _jyd_embed

def _jyd_bind(obj, code, host_name):
    f = _jyd_embed.get(host_name)
    obj.add_event_cb(lambda e: f(), code, None)
"""


_TOUCH_BIND_SRC = r"""
import drive

_jyd_touch_events = []

def _jyd_bind_touch(obj):
    def emit(phase):
        def callback(event):
            try:
                # The bundled evdev driver updates these values before LVGL
                # dispatches the event. Reading them avoids passing lv_event
                # / lv_indev objects through the MicroPython bridge.
                point_x = drive.mouse.hor_res - drive.mouse.x
                point_y = drive.mouse.ver_res - drive.mouse.y
                point_x = max(0, min(drive.mouse.hor_res - 1, point_x))
                point_y = max(0, min(drive.mouse.ver_res - 1, point_y))
                _jyd_touch_events.append("%s,%d,%d" % (
                    phase, int(point_x), int(point_y)))
            except Exception:
                pass
        return callback
    obj.add_event_cb(emit("pressed"), lv.EVENT.PRESSED, None)
    obj.add_event_cb(emit("moving"), lv.EVENT.PRESSING, None)
    obj.add_event_cb(emit("released"), lv.EVENT.RELEASED, None)

def _jyd_read_touch():
    if not _jyd_touch_events:
        return ""
    return _jyd_touch_events.pop(0)
"""

_bind_state = {"ready": False, "seq": 0}
_touch_state = {"ready": False}


#: show(img) 的 MP 侧控件托管。控件、缓冲签名都放 MP 侧，每帧一次
#: m.call 就完成"活着且在当前屏上？-> 重建 / 重设源 / 标脏"三选一。
#: 控件挂在活动屏上，屏被删（appfw 退出应用整屏 delete）后代理会抛
#: LvReferenceError，换屏后 get_screen() 不再是活动屏：两种情况都在当前
#: 活动屏上重建，而不是抱着死控件。依赖 jyd.image 注入的 _jyd_img_set
_SHOW_SRC = r"""
import lvgl as _jyd_lv

_jyd_show_widget = None
_jyd_show_sig = None

def _jyd_show_img(w, h, cf_name, addr, size):
    global _jyd_show_widget, _jyd_show_sig
    scr = _jyd_lv.screen_active()
    widget = _jyd_show_widget
    try:
        alive = widget is not None and widget.get_screen() is scr
    except Exception:            # 控件已随旧屏删除
        alive = False
    if not alive:
        widget = _jyd_lv.image(scr)
        _jyd_show_widget = widget
        _jyd_show_sig = None
    sig = (addr, w, h, cf_name)
    if sig != _jyd_show_sig:     # 首次 / 缓冲变了：重设 dsc
        _jyd_img_set(widget, w, h, cf_name, addr, size)
        _jyd_show_sig = sig
    else:                        # 内容更新：仅标脏触发重绘
        widget.invalidate()
"""

#: show(img) 的进程内状态：img 最近一次的 Image（保活：控件 dsc 零拷贝
#: 引用其像素）/ ready MP 侧辅助是否已注入
_show_state = {"img": None, "ready": False}


def _show_image(img):
    """show(img) 的控件托管：一个 lv.image 跟随当前活动屏，复用 + 标脏。"""
    from . import image as _image
    args = _image._lv_args(img)      # (w, h, cf 名, addr, size)，含 mode 校验
    rt = _image._ensure_mp()         # 先注入 _jyd_img_set 等辅助
    st = _show_state
    if not st["ready"]:
        rt.m.exec(_SHOW_SRC)
        st["ready"] = True
    rt.m.call("_jyd_show_img", *args)
    st["img"] = img


def show(image=None, fps=None):
    """可选的节奏控制 + 图像显示。三种用法：

        lv.show(30)              # 或 show(fps=30)：按帧率 sleep 限速
        lv.show(img)             # 显示/刷新 Image
        lv.show(img, 30)         # 两者兼有

    UI 渲染由 jyd-ui 线程自转，show 不驱动心跳（不调它 UI 也在跑），
    保留它是给循环控节奏。传 Image 时由本模块托管一个 lv.image
    控件：每次调用都把控件标脏（零拷贝共享像素，重绘即显示最新内
    容）；换了不同的像素缓冲（地址/尺寸/格式变化）自动重设控件源，
    仍复用同一控件。控件挂在当前活动屏上：换屏了、或所在屏被删了
    （appfw 退出应用会整屏删除），下一次 show 在新的活动屏上重建。
    相机预览就是 `while True: lv.show(camera.read_image())`。"""
    if image is not None:
        import _maix_image
        if isinstance(image, _maix_image.Image):
            _show_image(image)
        elif fps is None:
            image, fps = None, image       # show(30) 兼容：首参是帧率
        else:
            raise TypeError(
                "show() 第一个参数应为 Image 或 fps 数字，拿到: %r"
                % (image,))
    _runtime.runtime().show(fps)


def bind(obj, event_code, fn):
    """把 LVGL 事件绑定到 CPython 无参回调。

    obj/event_code 用本模块代理（如 lv.EVENT.CLICKED），fn 是普通
    CPython callable，不接收事件对象（事件对象无法跨桥）。回调在
    jyd-ui 线程的 tick 栈里执行（回调里可继续调 lv；与其他线程共享
    的状态自行注意并发）。"""
    rt = _runtime.runtime().ensure_display()
    if not _bind_state["ready"]:
        rt.m.exec(_BIND_SRC)
        _bind_state["ready"] = True
    _bind_state["seq"] += 1
    name = "_jyd_cb_%d" % _bind_state["seq"]   # 唯一名，避免同名回调互相覆盖
    rt.m.register(fn, name=name)   # 经转交队列注册：宿主函数表无锁
    rt.m.proxy_call("_jyd_bind", obj, event_code, name)


def bind_touch(obj):
    """Collect pointer events from one LVGL object.

    The MicroPython side owns the LVGL event object because it cannot cross
    the CPython bridge. Call :func:`read_touch` after ``show()`` to obtain
    ``("pressed"|"moving"|"released", x, y)`` integer events.
    """
    rt = _runtime.runtime().ensure_display()
    if not _touch_state["ready"]:
        rt.m.exec(_TOUCH_BIND_SRC)
        _touch_state["ready"] = True
    rt.m.proxy_call("_jyd_bind_touch", obj)


def read_touch():
    """Return the next queued ``(phase, x, y)`` touch event, or ``None``."""
    rt = _runtime.runtime().ensure_display()
    value = rt.m.proxy_call("_jyd_read_touch")
    if not value:
        return None
    phase, x, y = value.split(",", 2)
    return phase, int(x), int(y)


# ---- 批量绘制池：线段 / 标签 ----

#: MP 侧实现。每个池一个 id，注册表 _jyd_pools 按 id 存控件列表，支持
#: 同时存在多个池（一个应用框 + 骨架、另一个应用柱状条互不干扰）。
#: parent 删除（appfw 退出应用整屏 delete）时注册项随 DELETE 事件移除，
#: 之后再对该 id 下发只是空操作，不会碰已删除的 LVGL 对象。
#: 线段 payload 是 int16 LE 扁平 [x1,y1,x2,y2]*n；标签 payload 每条
#: int16 x, int16 y, uint16 长度, utf-8 文本。多余控件隐藏，空 payload 全隐
_POOL_SRC = r"""
import struct as _jyd_struct
import lvgl as _jyd_lv

_jyd_pools = {}
_jyd_pool_seq = [0]

def _jyd_pool_register(parent, widgets):
    _jyd_pool_seq[0] += 1
    pid = _jyd_pool_seq[0]
    _jyd_pools[pid] = widgets
    parent.add_event_cb(lambda e: _jyd_pools.pop(pid, None),
                        _jyd_lv.EVENT.DELETE, None)
    return pid

def _jyd_lines_new(parent, n, color_hex, width):
    lines = []
    for i in range(n):
        pts = [{'x': 0, 'y': 0}, {'x': 0, 'y': 0}]
        ln = _jyd_lv.line(parent)
        ln.set_style_line_color(_jyd_lv.color_hex(color_hex), 0)
        ln.set_style_line_width(width, 0)
        ln.set_points(pts, 2)
        ln.add_flag(_jyd_lv.obj.FLAG.HIDDEN)
        lines.append((ln, pts))
    return _jyd_pool_register(parent, lines)

def _jyd_lines_set(pid, payload):
    lines = _jyd_pools.get(pid)
    if lines is None:
        return
    n = len(payload) // 8
    for i in range(len(lines)):
        ln, pts = lines[i]
        if i < n:
            x1, y1, x2, y2 = _jyd_struct.unpack_from('<hhhh', payload, i * 8)
            pts[0]['x'] = x1
            pts[0]['y'] = y1
            pts[1]['x'] = x2
            pts[1]['y'] = y2
            ln.set_points(pts, 2)
            ln.remove_flag(_jyd_lv.obj.FLAG.HIDDEN)
        else:
            ln.add_flag(_jyd_lv.obj.FLAG.HIDDEN)

def _jyd_bars_set(pid, x0, step, base, heights):
    lines = _jyd_pools.get(pid)
    if lines is None:
        return
    n = min(len(lines), len(heights) // 2)
    for i in range(n):
        h = _jyd_struct.unpack_from('<h', heights, i * 2)[0]
        ln, pts = lines[i]
        x = x0 + i * step
        pts[0]['x'] = x
        pts[0]['y'] = base
        pts[1]['x'] = x
        pts[1]['y'] = base - h
        ln.set_points(pts, 2)
        ln.remove_flag(_jyd_lv.obj.FLAG.HIDDEN)
    for i in range(n, len(lines)):
        lines[i][0].add_flag(_jyd_lv.obj.FLAG.HIDDEN)

def _jyd_labels_new(parent, n, color_hex):
    labels = []
    for i in range(n):
        lb = _jyd_lv.label(parent)
        lb.set_style_text_color(_jyd_lv.color_hex(color_hex), 0)
        lb.set_text("")
        lb.add_flag(_jyd_lv.obj.FLAG.HIDDEN)
        labels.append(lb)
    return _jyd_pool_register(parent, labels)

def _jyd_labels_set(pid, payload):
    labels = _jyd_pools.get(pid)
    if labels is None:
        return
    off = 0
    i = 0
    while i < len(labels) and off + 6 <= len(payload):
        x, y, n = _jyd_struct.unpack_from('<hhH', payload, off)
        off += 6
        lb = labels[i]
        lb.set_text(bytes(payload[off:off + n]).decode())
        lb.set_pos(x, y)
        lb.remove_flag(_jyd_lv.obj.FLAG.HIDDEN)
        off += n
        i += 1
    for j in range(i, len(labels)):
        labels[j].add_flag(_jyd_lv.obj.FLAG.HIDDEN)
"""

_pool_state = {"ready": False}


def _ensure_pool_mp():
    rt = _runtime.runtime().ensure_display()
    if not _pool_state["ready"]:
        rt.m.exec(_POOL_SRC)
        _pool_state["ready"] = True
    return rt


def _i16(v):
    v = int(v)
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


class LinePool:
    """n 条 lv.line 的批量绘制。parent 是 LVGL 父对象代理（通常是应用
    screen），线段控件随 parent 一起删除，之后再 draw 只是空操作。

        pool = lv.LinePool(scr, 80)                 # 容量 80 条线段
        pool.draw(boxes=[(x1, y1, x2, y2)],          # 每框 4 条
                  lines=[(x1, y1, x2, y2)],          # 骨架/多边形边
                  points=[(x, y)], point_size=4)     # 每点 2 条（十字）
        pool.draw()                                  # 全部隐藏

    一帧一次桥调用；超出容量的部分丢弃，用不到的控件隐藏。"""

    def __init__(self, parent, capacity, color=0x00E676, width=3):
        rt = _ensure_pool_mp()
        self._m = rt.m
        self._n = int(capacity)
        # parent 是代理对象，创建走 proxy_call 编组；之后全标量走 m.call 快速路径
        self._id = rt.m.proxy_call("_jyd_lines_new", parent, self._n,
                                   int(color), int(width))

    @property
    def capacity(self):
        return self._n

    def draw(self, lines=(), boxes=(), points=(), point_size=4):
        # 展开顺序 框 -> 线 -> 点：容量不够时先保住框
        coords = []
        for x1, y1, x2, y2 in boxes:
            coords += (x1, y1, x2, y1, x2, y1, x2, y2,
                       x2, y2, x1, y2, x1, y2, x1, y1)
        for x1, y1, x2, y2 in lines:
            coords += (x1, y1, x2, y2)
        r = point_size
        for x, y in points:
            coords += (x - r, y, x + r, y, x, y - r, x, y + r)
        del coords[self._n * 4:]
        payload = struct.pack("<%dh" % len(coords), *[_i16(v) for v in coords])
        self._m.call("_jyd_lines_set", self._id, payload)

    def set_bars(self, x0, step, base, heights):
        """柱状图：第 i 条画 (x0+i*step, base) -> (x0+i*step, base-h[i])。

        heights 是 int16 LE 的 bytes（如 np.asarray(h, dtype='<i2').tobytes()）；
        条数超过容量的丢弃，不足的隐藏。"""
        self._m.call("_jyd_bars_set", self._id, int(x0), int(step), int(base),
                     bytes(heights))


class LabelPool:
    """n 个 lv.label 的批量文本（检测框标签、分类列表这类小文本）。

        labels = lv.LabelPool(scr, 6)
        labels.set_all([(x, y, "person 0.93"), ...])   # 一次桥调用
        labels.set_all([])                             # 全部隐藏

    parent 删除后再 set_all 只是空操作。"""

    def __init__(self, parent, capacity, color=0xFFFFFF):
        rt = _ensure_pool_mp()
        self._m = rt.m
        self._n = int(capacity)
        self._id = rt.m.proxy_call("_jyd_labels_new", parent, self._n, int(color))

    @property
    def capacity(self):
        return self._n

    def set_all(self, items):
        parts = []
        for x, y, text in list(items)[:self._n]:
            data = str(text).encode("utf-8")
            parts.append(struct.pack("<hhH", _i16(x), _i16(y), len(data)) + data)
        self._m.call("_jyd_labels_set", self._id, b"".join(parts))


def _proxy_set_src(self, src):
    """代理对象的 set_src 增强：直接传 _maix_image 的 Image 时走零拷贝
    dsc 路径（见 jyd.image._lv_set_src），其余参数照旧跨桥转发。"""
    import _maix_image
    if isinstance(src, _maix_image.Image):
        from . import image as _image
        return _image._lv_set_src(self, src)
    client = object.__getattribute__(self, "_client")
    path = object.__getattribute__(self, "_path")
    return client.proxy_call(path + ".set_src", src)


# 挂到代理类上：类属性查找先于 __getattr__ 兜底，只拦截 set_src 这个名字
_Attribute.set_src = _proxy_set_src


def __getattr__(name):
    rt = _runtime.runtime().ensure_display()
    return getattr(rt.m.env.lv, name)
