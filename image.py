"""jyd.image：图像处理（_maix_image 直通）+ Frame / LVGL / OSD 互通辅助。

new()/load() 及 Image 的全部方法（find_qrcodes/find_barcodes/find_lines/
find_blobs/find_apriltags/...）与 _maix_image 完全一致，本模块只做转发；
另提供：

    from_frame(frame)   VPSS Frame 的 Y 平面 -> 零拷贝灰度 Image
    to_lv(img, parent)  Image -> 嵌入 LVGL 的 image 控件（零拷贝共享像素），
                        也可方法式调用：img.to_lv(lv.screen_active())
    show(img)           Image -> 双缓冲 OSD 直绘图层（盖在 LVGL UI 之上），
                        也可方法式调用：img.show()；每次调用提交一帧并把
                        img 重定向到新的后台画布，之后须整幅重画（见 show）

    from jyd import camera, image
    cam = camera.live()
    with cam.read() as frame:
        img = image.from_frame(frame)   # 借帧内存，处理须在 with 块内完成
        codes = img.find_qrcodes()
"""

import ctypes
import sys

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


def _lv_args(img):
    """Image -> (w, h, cf 名, addr, size)，含 mode 校验。"""
    if img is _show["img"]:
        raise RuntimeError(
            "这张 Image 已被 image.show() 收编为 OSD 直绘画布（像素地址"
            "每帧翻转），不能再作 LVGL 控件源；直接 image.show(img) 刷新")
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
    - 像素内存归宿主侧所有，必须比控件活得久。camera.read_image() 的
      缓冲由 Camera 常驻复用（同尺寸不换地址）：预览场景控件建一次，
      之后每帧 read_image() 后 widget.invalidate() 即可刷新画面。
    """
    args = _lv_args(img)
    rt = _ensure_mp()
    return rt.m.proxy_call("_jyd_img_create", parent, *args)


def _lv_set_src(widget, img):
    """jyd.lv 的 set_src 桥接：给已有 lv 控件代理设 Image 源（零拷贝）。"""
    args = _lv_args(img)
    rt = _ensure_mp()
    rt.m.proxy_call("_jyd_img_set", widget, *args)


# ---- image.show：双缓冲 OSD 直绘图层 ----

#: OSD handle / RGN layer：202/layer=1 是 LVGL UI 层（见 _runtime），
#: 直绘层用 203、layer=2，盖在 UI 之上
_SHOW_HANDLE = 203
_SHOW_LAYER = 2

#: show 的进程内状态：osd 区域对象 / bufs 两块常驻画布 / back 当前后台
#: 下标 / img 被收编的 Image（保活 + 身份判定）/ pos 图层位置 /
#: registered 退出清理是否已登记
_show = {"osd": None, "bufs": None, "back": 0, "img": None,
         "pos": (0, 0), "registered": False}


def _destroy_show_osd():
    osd, _show["osd"] = _show["osd"], None
    _show["bufs"] = None
    if osd is not None:
        try:
            osd.destroy()
        except Exception as e:
            print("jyd: image.show osd destroy error: %s" % e)


def _teardown_show():
    """进程退出清理：脱钩绑定图 -> 销毁 OSD（幂等）。经 runtime 的 exit
    回调执行，时序在 VO 关闭之后、LVGL OSD（202）销毁之前。"""
    img, _show["img"] = _show["img"], None
    if img is not None:
        img.new(size=(2, 2), mode="RGBA")
    _destroy_show_osd()
    _show["registered"] = False   # runtime 的回调表已清空，之后需重新登记


def _move_show_osd(x, y):
    """显式传了 x/y 才移动图层（None 保持原位）。"""
    if x is None and y is None:
        return
    cur = _show["pos"]
    pos = (cur[0] if x is None else x, cur[1] if y is None else y)
    if pos != cur:
        _show["osd"].move_to(pos[0], pos[1])
        _show["pos"] = pos


def _adopt(img, x, y):
    """收编路径：建/重建 OSD -> 拷入 img 当前内容 -> 翻转 -> 重定向。

    覆盖三种入口：首次 show、换了一个 Image 对象、用户 new/resize/
    convert 把绑定图换离了画布。首帧靠一次 memmove 保住内容，之后
    都走 show() 的零拷贝快路径。"""
    import tdl_py
    rt = _runtime.runtime().wait_display_ready()

    old, _show["img"] = _show["img"], None
    if old is not None and old is not img:
        old.new(size=(2, 2), mode="RGBA")

    w, h = img.width, img.height
    osd, bufs = _show["osd"], _show["bufs"]
    if osd is not None and (bufs[0].width, bufs[0].height) != (w, h):
        _destroy_show_osd()   # 尺寸变了：整层重建
        osd = None
    if osd is None:
        tdl_py.rgn_destroy(_SHOW_HANDLE, 1, 0)   # 清强杀残留的同名 handle
        pos = (_show["pos"][0] if x is None else x,
               _show["pos"][1] if y is None else y)
        osd = tdl_py.Osd(handle=_SHOW_HANDLE, width=w, height=h,
                         canvas_count=2)
        osd.create()
        try:
            osd.attach(group=1, channel=0, x=pos[0], y=pos[1],
                       layer=_SHOW_LAYER)
            back, front = osd.persistent_pair()
            if back.stride != w * 4 or front.stride != w * 4:
                raise RuntimeError(
                    "OSD 画布 stride=%d != 宽 %d*4，行尾有硬件对齐填充，"
                    "零拷贝直绘不成立；把宽度改成 16 像素的倍数再试"
                    % (back.stride, w))
            osd.set_visible(True)
        except Exception:
            try:
                osd.destroy()
            except Exception:
                pass
            raise
        _show["osd"] = osd
        _show["bufs"] = (back, front)
        _show["back"] = 0
        _show["pos"] = pos
        if not _show["registered"]:
            _show["registered"] = True
            rt.on_exit(_teardown_show)
    else:
        _move_show_osd(x, y)

    bufs = _show["bufs"]
    ctypes.memmove(bufs[_show["back"]].addr, img.to_addr(), w * h * 4)
    osd.update()
    _show["back"] ^= 1
    back = bufs[_show["back"]].addr
    img.new(size=(w, h), mode="RGBA", addr=back)
    # 只在收编时校验一次重定向生效（快路径的进入条件已隐含证明）：
    # 旧 .so 不支持 Image.new(addr=) 时报错，而不是静默退化成每帧 memmove
    if img.to_addr() != back:
        raise RuntimeError(
            "Image.new(addr=) 重定向未生效（捆绑的 _maix_image.so 过旧？），"
            "image.show 依赖该定制接口")
    _show["img"] = img
    rt._ensure_vo()
    return img


def show(img, x=None, y=None):
    """把 Image 提交上屏：双缓冲 OSD 直绘图层（盖在 LVGL UI 之上）。

    首次调用自动创建与 img 等尺寸的双缓冲 OSD，把 img 当前内容拷进
    后台画布、翻转上屏，再用 img.new(addr=) 把**同一个对象**重定向到
    新的后台画布——此后 img 的像素就是 OSD 显存，绘制零拷贝，每次
    show() 只做翻转 + 重定向：

        img = image.new(size=(720, 480), mode="RGBA")
        while True:
            img.clear()                 # 双缓冲：每帧必须整幅重画
            img.draw_rectangle(100, 100, 300, 260,
                               color=(0, 0, 255, 255), thickness=3)
            image.show(img)             # 或 img.show()

    约束与说明：

    - mode 只支持 "RGBA"（OSD 画布是 ARGB8888）；相机画面显示请走
      lv.show(img)。画布内存是 B,G,R,A 字节序：要屏幕红色传
      color=(0, 0, 255, 255)。
    - 双缓冲两块画布内容独立，每帧必须整幅重画（先 clear()），
      否则隔帧残影。
    - 只提交本图层；LVGL UI 由 jyd-ui 线程自转，无需再调 lv.show()。
      没有限速，节奏自己控（如 time.sleep 或跟随相机取帧）。
    - x/y 仅在显式传入时移动图层，缺省保持原位（创建时默认 (0, 0)）。
    - 换一个 Image 或尺寸变化会自动重建 OSD 收编新图，被顶掉的旧图
      重定向到自有小缓冲安全脱钩（内容不保留）。
    - 宽度需满足硬件 stride 对齐（建议 16 像素的倍数），不满足时报错。
    """
    if not isinstance(img, _mi.Image):
        raise TypeError(
            "image.show() 只接受 image 模块的 Image，拿到: %r" % (img,))
    if img.mode != "RGBA":
        raise ValueError(
            "image.show 只支持 mode='RGBA'（OSD 画布是 ARGB8888），"
            "拿到 %r；相机画面显示请用 lv.show(img)" % (img.mode,))
    lv_mod = sys.modules.get(__package__ + ".lv") if __package__ else None
    if lv_mod is not None and lv_mod._show_state.get("img") is img:
        raise RuntimeError(
            "这张 Image 正被 lv.show() 的托管控件引用，不能再走 "
            "image.show() 直绘：两条路径会争用同一块像素内存")
    st = _show
    if (img is st["img"] and st["osd"] is not None
            and img.to_addr() == st["bufs"][st["back"]].addr):
        # 快路径：img 就画在当前后台画布上，翻转 + 重定向即可
        _move_show_osd(x, y)
        st["osd"].update()
        st["back"] ^= 1
        img.new(size=(img.width, img.height), mode="RGBA",
                addr=st["bufs"][st["back"]].addr)
        _runtime.runtime()._ensure_vo()
        return img
    return _adopt(img, x, y)


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
