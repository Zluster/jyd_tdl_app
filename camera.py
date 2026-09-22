"""jyd.camera：VPSS 通道取帧。

通道是板级固定配置（dual-OS 小核负责采集），按预设工厂取用：

    前摄采集 grp0 / 显示 grp1：
        rgb()       grp0/ch0  720x480   BGR888_PLANAR（Python 图像处理）
        ai()        grp0/ch1  640x640   RGB888_PLANAR（NN 推理输入，letterbox 黑边）
        live()      grp0/ch2  720x480   NV12（与屏幕预览同源，CV 识别常用）
        sub_rgb()   grp0/ch3  320x240   BGR888_PLANAR
        screen()    grp1/ch0  720x480   NV12（显示处理通道，送 VO 的画面）

    后摄采集 grp3（规格为布局名义值，实际以小核配置为准）：
        rear_rgb()  ch0    rear_ai()   ch1    rear_live()  ch2    rear_sub_rgb()  ch3

timeout_ms 仅该通道首次创建生效。首次创建即 open（失败立刻暴露），进程退出自动 close。通道
被其他进程占用（launcher/ai_cycle 在跑）时 open 会失败。

取帧只有一个入口 read()，返回的 Frame **既能喂 NPU，也能当 Image 用**
（模块级 camera.read() 走 ai 通道，rear=True 切后摄；其他通道用工厂实例）：

    with camera.read() as frame:          # ai 通道 640x640，原生零拷贝帧
        result = model.run(frame)         # NPU 直接吃原生帧，不做任何转换
        codes = frame.find_qrcodes()      # 首次用到 Image 能力才转成紧凑图
        lv.show(frame)                    # 显示同样直接接受

    with camera.rgb().read() as frame:    # 720x480 通道：同一帧既推理又当图
        boxes = model.run(frame).boxes
        frame.draw_rectangle(...)         # 只改 Image 视图，NPU 看的仍是原帧
        lv.show(frame)

    img = camera.read_image()             # 兼容别名：直接拿 Image，帧立即归还
    img = live().read_image()             # NV12/NV21 通道出 Y 平面灰度 Image

Image 能力必须在帧有效期内**首次**触发（转换要读帧内存）；转换出的
Image 是拷贝，到同相机下一次 read() 前有效。详见 Frame 的说明。

屏幕底层预览用 preview() 控制，三态："front" 前摄 / "rear" 后摄 /
"off" 遮挡（显示态自动把 screen/layer_bottom 背景透明，遮挡态置回
不透明）；后摄需小核已在跑 grp3 采集。
"""

import ctypes

import tdl_py

from . import _runtime

#: 统一 Frame 上直通原生 tdl_py.Frame 的名字；其余名字落到 Image 视图
_NATIVE_ATTRS = frozenset((
    "width", "height", "format", "sequence", "timestamp_us", "phys_addr",
    "plane_count", "valid", "addr", "size", "data", "strides",
    "plane_sizes", "plane_offsets", "plane", "copy_to",
))


class Frame:
    """camera.read() 的返回值：同一个对象既能喂 NPU，也能当 Image 用。

    - .frame  原生 tdl_py.Frame（VPSS 帧的零拷贝映射）。nn 推理走它，
              不做任何像素转换；width/height/format/strides/data/plane()
              等原生属性直接透传（直接调 tdl_py 底层接口时传 .frame）
    - .image  Image 视图：首次用到 find_*/draw_*/save/mode/to_addr 等
              Image 能力时，才经 Frame.copy_to 转成紧凑图（RGB 通道约
              1 MB，NV12 通道出灰度约 0.35 MB），之后缓存在本对象上；
              直接写 frame.find_qrcodes() 即可，不必显式取 .image。
              像素缓冲由 Camera 常驻复用、跨帧地址不变（lv.show 的控件
              复用与 to_lv 的"同尺寸不换地址"都靠它）

    生命周期与原生帧一致：出 with 块 / release() / 同相机下一次 read()
    都使原生帧失效。Image 能力必须在帧有效期内**首次**触发（转换要读
    帧内存），转换出的 Image 是拷贝、到同相机下一次 read() 前有效（之后
    缓冲被新帧覆盖）。在帧上画框只改 Image 视图，NPU 看到的仍是原始帧。
    """

    def __init__(self, camera, raw):
        self._camera = camera
        self._raw = raw
        self._image = None

    @property
    def frame(self):
        """原生 tdl_py.Frame。"""
        return self._raw

    @property
    def image(self):
        """Image 视图（首次访问触发转换，要求帧仍有效）。"""
        if self._image is None:
            if not self._raw.valid:
                raise RuntimeError(
                    "帧已释放（出了 with 块 / 已 release / 相机已取下一帧），"
                    "Image 能力必须在帧有效期内首次使用；要跨块使用，先在"
                    "块内触碰一次 frame.image")
            self._image = self._camera._to_image(self._raw)
        return self._image

    def release(self):
        """归还 VPSS 帧（幂等）。已转换出的 Image 视图不受影响。"""
        self._raw.release()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.release()
        return False

    def __getattr__(self, name):
        if name in _NATIVE_ATTRS:
            return getattr(self._raw, name)
        if name.startswith("_"):
            raise AttributeError(name)
        return getattr(self.image, name)   # Image 能力：首次触发转换

    def __repr__(self):
        return "<jyd.camera.Frame grp%d/ch%d %dx%d fmt=%d %s>" % (
            self._camera.group, self._camera.channel, self._raw.width,
            self._raw.height, self._raw.format,
            "image" if self._image is not None else "raw")


class Camera:
    """一个 VPSS 通道。用 rgb()/ai()/live()/screen()/rear_*() 等工厂取实例。"""

    def __init__(self, raw, key):
        self._cam = raw
        self._key = key
        self._group, self._channel = key
        self._held = None      # Frame.image 的像素缓冲（跨帧复用，地址不变）

    @property
    def group(self):
        return self._group

    @property
    def channel(self):
        return self._channel

    def read(self):
        """阻塞取一帧（最长 timeout_ms），返回统一 Frame：喂 nn 推理零拷贝，
        用到 Image 能力时才转成紧凑图（见 Frame）。

        务必用 `with cam.read() as frame:`——原生帧引用 VPSS 池内存，
        出块自动归还；同相机下一次 read 也会使上一帧失效。"""
        return Frame(self, self._cam.read())

    def read_image(self):
        """兼容别名：取一帧并立即转成 Image，原生帧随即归还 VPSS。

        等价于 `frame = cam.read(); img = frame.image; frame.release()`，
        返回的 Image 到同相机下一次 read()/read_image() 前有效。"""
        frame = self.read()
        try:
            return frame.image
        finally:
            frame.release()

    def _to_image(self, raw):
        """原生帧 -> 紧凑 Image（Frame.image 的转换实现）：

        - RGB888 / BGR888 / RGB888_PLANAR / BGR888_PLANAR：出 "RGB" 模式
          Image，内存按 _maix_image 约定排成 B,G,R 字节序（to_lv / save /
          颜色算法共用这一约定），约 1 MB
        - NV12 / NV21：出 Y 平面灰度 Image，约 0.35 MB
        - 其他格式抛 RuntimeError

        转换（剥 stride 填充、planar 交错、对调 R/B）由 tdl_py 的
        Frame.copy_to 在**持 GIL 的单次原生调用**里完成。像素缓冲由本
        对象常驻复用、跨帧地址不变，它很可能正是 LVGL 控件的零拷贝像素
        源，而 jyd-ui 线程的渲染同样持 GIL——单次原生调用才是与渲染互斥
        的依据，绝不能退回 Python 层原地多步改写（渲染线程会看到只换了
        一个颜色分量的半成品，动起来就是彩色重影）。"""
        from . import image
        w, h, fmt = raw.width, raw.height, raw.format
        if fmt in (tdl_py.FORMAT_NV12, tdl_py.FORMAT_NV21):
            mode, layout, bpp = "L", "gray", 1
        elif fmt in (tdl_py.FORMAT_RGB888, tdl_py.FORMAT_BGR888,
                     tdl_py.FORMAT_RGB888_PLANAR,
                     tdl_py.FORMAT_BGR888_PLANAR):
            mode, layout, bpp = "RGB", "bgr", 3
        else:
            raise RuntimeError(
                "grp%d/ch%d 帧格式 %d 不支持转成 Image（支持 RGB888/"
                "BGR888/RGB888_PLANAR/BGR888_PLANAR/NV12/NV21）"
                % (self._group, self._channel, fmt))
        buf = self._held
        if buf is None or len(buf) != w * h * bpp:
            buf = bytearray(w * h * bpp)
        raw.copy_to(buf, layout)
        addr = ctypes.addressof((ctypes.c_ubyte * len(buf)).from_buffer(buf))
        img = image.new(size=(w, h), mode=mode, addr=addr)
        self._held = buf
        return img

    def close(self):
        """释放通道（幂等）。一般不用手动调，进程退出自动清理。"""
        self._held = None      # 像素缓冲交 GC（按约定此刻失效）
        if self._cam is not None:
            cam, self._cam = self._cam, None
            _instances.pop(self._key, None)
            cam.close()

    def __repr__(self):
        state = "closed" if self._cam is None else "open"
        return "<jyd.camera.Camera grp%d/ch%d %s>" % (
            self._group, self._channel, state)


_instances = {}


def _get(factory, timeout_ms):
    raw = factory(timeout_ms=timeout_ms)   # 构造不碰硬件，open 才会
    key = (raw.group, raw.channel)
    cam = _instances.get(key)
    if cam is None:
        raw.open()   # 失败立刻抛（通道被占用/采集未就绪在这暴露）
        cam = Camera(raw, key)
        _instances[key] = cam
        _runtime.runtime().on_exit(cam.close)
    return cam


def read(rear=False, timeout_ms=1000):
    """从 ai 通道取一帧（640x640 RGB888_PLANAR，NN 推理输入），返回统一
    Frame：model.run(frame) 零拷贝推理，frame.find_qrcodes() / lv.show(frame)
    等 Image 能力按需转换。rear=True 走后摄 grp3/ch1，否则前摄 grp0/ch1。

    等价于 (rear_ai() if rear else ai()).read()：务必用
    `with camera.read() as frame:`——出块即失效，推理/首次 Image 转换都
    必须在块内完成。要 720x480 的帧同时推理和当图，用 camera.rgb().read()。"""
    cam = rear_ai(timeout_ms) if rear else ai(timeout_ms)
    return cam.read()


def read_image(rear=False, timeout_ms=1000):
    """兼容别名：从 rgb 通道取一帧并直接转成紧凑 RGB Image（720x480），
    原生帧立即归还。rear=True 走后摄 grp3/ch0，否则前摄 grp0/ch0。

    等价于 (rear_rgb() if rear else rgb()).read_image()。其他通道用
    工厂实例的 cam.read_image()（NV12/NV21 通道出 Y 平面灰度图）；
    转换规则见 Camera._to_image，生命周期见 Frame。"""
    cam = rear_rgb(timeout_ms) if rear else rgb(timeout_ms)
    return cam.read_image()


def preview(source="front"):
    """屏幕底层相机预览，三态开关：

    - "front"（或 False）：显示前摄 live（grp0/ch2 -> grp1 -> VO）
    - "rear" （或 True） ：显示后摄 live（grp3/ch2 -> grp1 -> VO）
    - "off"  （或 None） ：不显示（视频仍在底层流动以承载 UI 帧，
      仅用不透明底遮住）

    显示态自动把 lv 的 screen_active/layer_bottom 背景透明
    （bg_opa=0），遮挡态置回不透明（bg_opa=255）；screen_load 换屏后
    对新 screen 需重调一次。显示通路未建立时只记录期望（建链时生
    效）；已建立时立即生效，OSD/UI 不受影响。后摄画面要求小核已在
    跑 grp3 采集。"""
    _runtime.runtime().set_preview(source)


def to_screen(x, y, frame_width=640, frame_height=640):
    """ 帧 -> 屏幕坐标映射（仿 launcher/apps/ai 的 CoordMap）。

    推理帧与屏幕显示的都是同一 sensor 画面的等比嵌入：ai 640x640 上下
    黑边（内容 640x480，oy=80），屏幕 720x480 左右黑边（内容 640x480，
    ox=40）。映射 = 去帧黑边按内容归一，再加屏幕黑边；ai 帧数值下退化
    """
    src_w, src_h = 1600.0, 1200.0
    screen_w, screen_h = 720.0, 480.0

    frame_fit = min(frame_width / src_w, frame_height / src_h)
    frame_content_w, frame_content_h = src_w * frame_fit, src_h * frame_fit
    frame_offset_x = (frame_width - frame_content_w) * 0.5
    frame_offset_y = (frame_height - frame_content_h) * 0.5

    screen_fit = min(screen_w / src_w, screen_h / src_h)
    screen_content_w = src_w * screen_fit
    screen_content_h = src_h * screen_fit
    screen_offset_x = (screen_w - screen_content_w) * 0.5
    screen_offset_y = (screen_h - screen_content_h) * 0.5

    return (int((x - frame_offset_x) / frame_content_w * screen_content_w
                + screen_offset_x),
            int((y - frame_offset_y) / frame_content_h * screen_content_h
                + screen_offset_y))


# ---- 前摄 grp0 / 显示 grp1 ----

def rgb(timeout_ms=1000) -> Camera:
    """grp0/ch0，720x480 BGR888_PLANAR。"""
    return _get(tdl_py.VpssCamera.rgb, timeout_ms)

def ai(timeout_ms=1000) -> Camera:
    """grp0/ch1，640x640 RGB888_PLANAR（NN 推理输入）。"""
    return _get(tdl_py.VpssCamera.ai, timeout_ms)

def live(timeout_ms=1000) -> Camera:
    """grp0/ch2，720x480 NV12（预览同源）。"""
    return _get(tdl_py.VpssCamera.live, timeout_ms)

def sub_rgb(timeout_ms=1000) -> Camera:
    """grp0/ch3，320x240 BGR888_PLANAR。"""
    return _get(tdl_py.VpssCamera.sub_rgb, timeout_ms)

def screen(timeout_ms=1000) -> Camera:
    """grp1/ch0，720x480 NV12（显示处理通道，读到的是送 VO 的画面）。"""
    return _get(tdl_py.VpssCamera.screen, timeout_ms)


# ---- 后摄 grp3（规格为名义值，实际以小核配置为准） ----

def rear_rgb(timeout_ms=1000) -> Camera:
    """grp3/ch0，720x480 BGR888_PLANAR。"""
    return _get(tdl_py.VpssCamera.rear_rgb, timeout_ms)

def rear_ai(timeout_ms=1000) -> Camera:
    """grp3/ch1，640x640 RGB888_PLANAR（NN 推理输入）。"""
    return _get(tdl_py.VpssCamera.rear_ai, timeout_ms)

def rear_live(timeout_ms=1000) -> Camera:
    """grp3/ch2，720x480 NV12。"""
    return _get(tdl_py.VpssCamera.rear_live, timeout_ms)

def rear_sub_rgb(timeout_ms=1000) -> Camera:
    """grp3/ch3，320x240 BGR888_PLANAR。"""
    return _get(tdl_py.VpssCamera.rear_sub_rgb, timeout_ms)
