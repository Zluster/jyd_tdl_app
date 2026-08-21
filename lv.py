"""jyd.lv：嵌入 LVGL 的模块级代理。

lv.* 全部属性转发到 MicroPython 里的 lvgl 模块（首次访问自动初始化
显示通路：VPSS->VO 链路、OSD 双缓冲、嵌入解释器、触摸驱动），另有
两个宿主侧扩展函数：

    show(...)        推进一次 UI（真实流逝时间 tick + 渲染 + 触摸分发），
                     用户主循环每轮调用一次，三种用法：
                       show(fps)        仅限速（防纯 UI 循环 CPU 空转）
                       show(img)        显示 Image 并推进（见下）
                       show(img, fps)   两者兼有
                     传 img 时模块内部只创建一个 lv.image 控件并复用：
                     同一块像素缓冲只 invalidate() 标脏重绘，缓冲地址/
                     尺寸/格式变了才重设 dsc；Image 引用由模块保活
    bind(obj, code, fn)   LVGL 事件 -> CPython 无参回调
                          （事件对象不能跨桥，MicroPython 侧吞掉）

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
        lv.show(fps=30)      # 有相机 read 等阻塞源时可不限速：lv.show()

注意：默认 screen 背景透明（露出摄像头画面），要不透明底自己
set_style_bg_opa。所有 lv 调用（含回调里的）都必须在主线程——
worker 线程只做取帧/推理，不碰 UI（与 launcher 应用的约定一致）。
"""

from . import _runtime
from ._mpyc.proxy import Attribute as _Attribute

_BIND_SRC = r"""
import mpy_embed as _jyd_embed

def _jyd_bind(obj, code, host_name):
    f = _jyd_embed.get(host_name)
    obj.add_event_cb(lambda e: f(), code, None)
"""

_bind_state = {"ready": False, "seq": 0}


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
    """推进一次 UI。三种用法：

        lv.show(30)              # 或 show(fps=30)：仅按帧率限速
        lv.show(img)             # 显示/刷新 Image 并推进 UI
        lv.show(img, 30)         # 两者兼有

    传 Image 时由本模块托管唯一的 lv.image 控件：每次调用都把控件标脏
    （零拷贝共享像素，重绘即显示最新内容）；换了不同的像素缓冲（地址/
    尺寸/格式变化）自动重设控件源，仍复用同一控件。相机预览就是
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
    show() 的调用栈里执行。"""
    import mpy
    rt = _runtime.runtime().ensure_display()
    if not _bind_state["ready"]:
        rt.m.exec(_BIND_SRC)
        _bind_state["ready"] = True
    _bind_state["seq"] += 1
    name = "_jyd_cb_%d" % _bind_state["seq"]   # 唯一名，避免同名回调互相覆盖
    mpy.register(fn, name=name)
    rt.m.proxy_call("_jyd_bind", obj, event_code, name)


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
