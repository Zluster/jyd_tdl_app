"""jyd.image：图像处理（_maix_image 直通）+ Frame / LVGL 互通辅助。

new()/load() 及 Image 的全部方法（find_qrcodes/find_barcodes/find_lines/
find_blobs/find_apriltags/...）与 _maix_image 完全一致，本模块只做转发；
另提供：

    from_frame(frame)   VPSS Frame 的 Y 平面 -> 零拷贝灰度 Image
    to_lv(img, parent)  Image -> 嵌入 LVGL 的 image 控件（零拷贝共享像素），
                        也可方法式调用：img.to_lv(lv.screen_active())
    show(img)           Image 同步渲染上屏

凡接受 Image 的入口（to_lv / show / 控件 set_src / lv.show）也直接接受
camera.read() 返回的 Frame——自动取其 Image 视图（首次触发转换）。
camera.read() 的 Frame 本身就能直接调 find_*/draw_* 等 Image 方法。

    from jyd import camera, image
    cam = camera.live()
    with cam.read() as frame:
        img = image.from_frame(frame)   # 借帧内存，处理须在 with 块内完成
        codes = img.find_qrcodes()
"""

import _maix_image as _mi

from . import _runtime

#: Image.mode -> (lv.COLOR_FORMAT 名, 每像素字节数)
_LV_CF = {
    "L": ("L8", 1),
    "RGB": ("RGB888", 3),
    "RGBA": ("ARGB8888", 4),
    "RGB16": ("RGB565", 2),
}

#: MP 侧辅助：uctypes 把宿主像素地址包成零拷贝视图建 dsc，设给控件。
#: dsc 必须存进注册表——LVGL C 侧只保存裸指针，dsc 被 MP GC 回收即野
#: 指针；换 src 时旧 dsc 随替换释放，控件 DELETE 事件时清理条目
_TO_LV_SRC = r"""
import lvgl as _jyd_lv
import uctypes as _jyd_uctypes

_jyd_img_dscs = {}

def _jyd_img_set(img, w, h, cf_name, addr, size):
    dsc = _jyd_lv.image_dsc_t({
        "header": {"w": w, "h": h,
                   "cf": getattr(_jyd_lv.COLOR_FORMAT, cf_name)},
        "data_size": size,
        "data": _jyd_uctypes.bytearray_at(addr, size),
    })
    first = id(img) not in _jyd_img_dscs
    _jyd_img_dscs[id(img)] = dsc
    img.set_src(dsc)
    if first:
        img.add_event_cb(lambda e: _jyd_img_dscs.pop(id(img), None),
                         _jyd_lv.EVENT.DELETE, None)

def _jyd_img_create(parent, w, h, cf_name, addr, size):
    if parent is None:
        parent = _jyd_lv.screen_active()
    img = _jyd_lv.image(parent)
    _jyd_img_set(img, w, h, cf_name, addr, size)
    return img
"""

_to_lv_state = {"ready": False}


def _as_image(obj):
    """Image 原样返回；camera.read() 的 Frame 取其 Image 视图（首次触发
    转换）；其他类型返回 None，由调用方按自己的语境报错。"""
    if isinstance(obj, _mi.Image):
        return obj
    from . import camera            # 惰性：camera 反向惰性引用本模块
    if isinstance(obj, camera.Frame):
        return obj.image
    return None


def _lv_args(obj):
    """Image（或 camera.read() 的 Frame）-> (w, h, cf 名, addr, size)，
    含 mode 校验。"""
    img = _as_image(obj)
    if img is None:
        raise TypeError(
            "需要 Image 或 camera.read() 返回的 Frame，拿到: %r" % (obj,))
    cf = _LV_CF.get(img.mode)
    if cf is None:
        raise ValueError("不支持的 Image.mode: %r（支持 %s）"
                         % (img.mode, "/".join(sorted(_LV_CF))))
    cf_name, bpp = cf
    w, h = img.width, img.height
    return w, h, cf_name, img.to_addr(), w * h * bpp


def _ensure_mp():
    """确保显示通路已建、MP 侧辅助已注入，返回 runtime。"""
    rt = _runtime.runtime().ensure_display()
    if not _to_lv_state["ready"]:
        rt.m.exec(_TO_LV_SRC)
        _to_lv_state["ready"] = True
    return rt


def to_lv(img, parent=None):
    """Image -> 嵌入 LVGL 的 image 控件（零拷贝：LVGL 直接渲染 Image 的
    像素内存，不复制；首次调用自动初始化显示通路）。

    - parent：LVGL 父对象代理，缺省 lv.screen_active()
    - 返回 lv.image 控件代理，可继续 center()/set_pos()/delete() 等
    - mode 支持 L/RGB/RGBA/RGB16，对应 L8/RGB888/ARGB8888/RGB565
    - 等价写法：w = lv.image(parent); w.set_src(img)（见 jyd.lv）
    - 也可直接传 camera.read() 的 Frame（取其 Image 视图）
    - 像素内存归宿主侧所有，必须比控件活得久。相机帧的 Image 缓冲由
      Camera 常驻复用（同尺寸不换地址）：预览场景控件建一次即可。
      但每帧刷新请用 lv.show(img)，不要只 widget.invalidate()：渲染在
      jyd-ui 线程异步进行，只标脏的话下一次 read() 会先于重绘把缓冲
      覆盖掉，画上去的内容根本来不及上屏；lv.show(img) 是同步交接，
      返回即已渲染。自己管控件的话在 invalidate() 之后调 lv.refr_now(None)。
    """
    args = _lv_args(img)
    rt = _ensure_mp()
    return rt.m.proxy_call("_jyd_img_create", parent, *args)


def _lv_set_src(widget, img):
    """jyd.lv 的 set_src 桥接：给已有 lv 控件代理设 Image 源（零拷贝）。"""
    args = _lv_args(img)
    rt = _ensure_mp()
    rt.m.proxy_call("_jyd_img_set", widget, *args)


def show(img):
    """把 Image 同步渲染上屏，等价于 lv.show(img)（见 jyd.lv.show）。
    也接受 camera.read() 的 Frame（取其 Image 视图）。

    lv 模块托管一个 lv.image 控件复用：零拷贝共享 img 的像素、标脏后
    当场渲染，返回时这一帧已经上屏，之后覆盖/重画 img 都安全。mode
    支持 L/RGB/RGBA/RGB16；显示画布是 B,G,R,A 字节序，要屏幕红色传
    color=(0, 0, 255, ...)。

        with camera.rgb().read() as frame:
            frame.draw_rectangle(100, 100, 300, 260, color=(0, 0, 255), thickness=3)
            image.show(frame)       # 或 frame.show()
    """
    image = _as_image(img)
    if image is None:
        raise TypeError(
            "image.show() 只接受 Image 或 camera.read() 返回的 Frame，"
            "拿到: %r" % (img,))
    from . import lv as _lv       # 惰性导入：lv 模块反向惰性引用本模块
    _lv.show(image)


# pybind 堆类型可挂方法：img.to_lv(parent) 与 to_lv(img, parent) 等价，
# img.show() 与 show(img) 同理。Python 侧类注册名是 Image
# （class_<maix_image>(mo, "Image")，maix_image 是 C++ 类型名）
_mi.Image.to_lv = to_lv
_mi.Image.show = show


def from_frame(frame, mode="L"):
    """NV12/NV21 Frame -> 零拷贝灰度 Image（Y 平面视图）。

    Image 借用帧内存，frame 释放（出 with 块 / 下一次 read）后失效。
    行有填充（stride != width）时零拷贝视图不成立，抛 RuntimeError。"""
    if frame.strides[0] != frame.width:
        raise RuntimeError(
            "stride %d != width %d，存在行填充，零拷贝灰度视图不成立"
            % (frame.strides[0], frame.width))
    return _mi.new(size=(frame.width, frame.height), mode=mode,
                   addr=frame.addr)


def __getattr__(name):
    return getattr(_mi, name)


def __dir__():
    return sorted(set(globals()) | set(dir(_mi)))
