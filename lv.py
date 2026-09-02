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
                     传 img 时模块内部只创建一个 lv.image 控件并复用：
                     同一块像素缓冲只 invalidate() 标脏重绘，缓冲地址/
                     尺寸/格式变了才重设 dsc；Image 引用由模块保活
    bind(obj, code, fn)   LVGL 事件 -> CPython 无参回调
                          （事件对象不能跨桥，MicroPython 侧吞掉）
    bind_touch(obj)       在对象上采集按下、移动和松开的触摸事件
    read_touch()          读取一条 (phase, x, y) 触摸事件

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


#: show(img) 托管的唯一 lv.image 控件：widget 控件代理 / img 最近一次
#: 的 Image（保活：dsc 零拷贝引用其像素）/ sig 缓冲签名（变了才重设 dsc）
_show_state = {"widget": None, "img": None, "sig": None}


def _show_image(img):
    """show(img) 的控件托管：进程内只建一个 lv.image，复用 + 标脏。"""
    from . import image as _image
    sig = (img.to_addr(), img.width, img.height, img.mode)
    st = _show_state
    if st["widget"] is None:
        st["widget"] = _image.to_lv(img)   # 首次：创建唯一控件并设源
    elif sig != st["sig"]:
        st["widget"].set_src(img)          # 缓冲变了：同一控件重设 dsc
    else:
        st["widget"].invalidate()          # 内容更新：仅标脏触发重绘
    st["img"] = img
    st["sig"] = sig


def show(image=None, fps=None):
    """可选的节奏控制 + 图像显示。三种用法：

        lv.show(30)              # 或 show(fps=30)：按帧率 sleep 限速
        lv.show(img)             # 显示/刷新 Image
        lv.show(img, 30)         # 两者兼有

    UI 渲染由 jyd-ui 线程自转，show 不驱动心跳（不调它 UI 也在跑），
    保留它是给循环控节奏。传 Image 时由本模块托管唯一的 lv.image
    控件：每次调用都把控件标脏（零拷贝共享像素，重绘即显示最新内
    容）；换了不同的像素缓冲（地址/尺寸/格式变化）自动重设控件源，
    仍复用同一控件。相机预览就是
    `while True: lv.show(camera.read_image())`。"""
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
