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

取帧只有一个入口 read()，返回的 Frame **既能喂 NPU，也能当 Image 用**。
模块级 camera.read() 会紧邻读取同侧的两个通道：NPU 能力用 ai 通道
640x640 原生帧，Image 能力用 rgb 通道 720x480 紧凑图；rear=True 切后摄。
具体通道工厂的 read() 仍只读取自身通道，Image 能力首次使用时按需转换：

    with camera.read() as frame:
        result = model.run(frame)         # ai 通道 640x640 原生帧，零拷贝
        codes = frame.find_qrcodes()      # rgb 通道 720x480 Image
        lv.show(frame)                    # 显示 720x480，不会出现 640 方图偏移

    with camera.rgb().read() as frame:    # 720x480 通道：同一帧既推理又当图
        boxes = model.run(frame).boxes
        frame.draw_rectangle(...)         # 只改 Image 视图，NPU 看的仍是原帧
        lv.show(frame)

    img = camera.read_image()             # 兼容别名：直接拿 Image，帧立即归还
    img = live().read_image()             # NV12/NV21 通道出 Y 平面灰度 Image

模块级 camera.read() 已经预装 RGB Image，不存在延迟转换；具体通道实例
read() 的 Image 能力仍是懒转换，必须在帧有效期内首次触发。详见 Frame。

屏幕底层预览用 preview() 控制，三态："front" 前摄 / "rear" 后摄 /
"off" 遮挡（显示态自动把 screen/layer_bottom 背景透明，遮挡态置回
不透明）；后摄需小核已在跑 grp3 采集。
"""

import ctypes

import tdl_py

from . import _runtime
from .core import device_config

#: 统一 Frame 上直通原生 tdl_py.Frame 的名字；其余名字落到 Image 视图
_NATIVE_ATTRS = frozenset((
    "width", "height", "format", "sequence", "timestamp_us", "phys_addr",
    "plane_count", "valid", "addr", "size", "data", "strides",
    "plane_sizes", "plane_offsets", "plane", "copy_to",
))


class Frame:
    """read() 的返回值：同一个对象既能喂 NPU，也能当 Image 用。

    - .frame  原生 tdl_py.Frame（VPSS 帧的零拷贝映射）。nn 推理走它，
              不做任何像素转换；width/height/format/strides/data/plane()
              等原生属性直接透传（直接调 tdl_py 底层接口时传 .frame）
    - .image  Image 视图。模块级 camera.read() 预装同侧 rgb 通道的
              720x480 图；具体通道实例（camera.ai().read() 等）则在
              首次用到 find_*/draw_*/save/mode/to_addr 时，才把本通道
              Frame.copy_to 成紧凑图。直接写 frame.find_qrcodes() 即可，
              不必显式取 .image。
              像素缓冲由 Camera 常驻复用、跨帧地址不变（lv.show 的控件
              复用与 to_lv 的"同尺寸不换地址"都靠它）

    生命周期与原生帧一致：出 with 块 / release() / 同相机下一次 read()
    都使原生帧失效。懒转换的 Image 必须在帧有效期内首次触发；模块级
    camera.read() 的 RGB Image 已预装，无此限制。Image 是拷贝、到对应
    rgb Camera 下一次 read() 前有效（之后缓冲被新帧覆盖）。在帧上画框
    只改 Image 视图，NPU 看到的仍是原始 ai 帧。
    """

    def __init__(self, camera, raw, image=None, image_camera=None):
        self._camera = camera
        self._raw = raw
        self._image = image
        self._image_camera = image_camera or camera

    @property
    def frame(self):
        """原生 tdl_py.Frame。"""
        return self._raw

    @property
    def image(self):
        """Image 视图。模块级 camera.read() 已预装 720x480 RGB 图；
        具体通道实例的 read() 首次访问才转换，且要求原生帧仍有效。"""
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
        image_desc = ("image=grp%d/ch%d %dx%d" % (
            self._image_camera.group, self._image_camera.channel,
            self._image.width, self._image.height)
            if self._image is not None else "image=lazy")
        return "<jyd.camera.Frame grp%d/ch%d %dx%d fmt=%d %s>" % (
            self._camera.group, self._camera.channel, self._raw.width,
            self._raw.height, self._raw.format, image_desc)


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
_DEFAULT_SOURCE = object()


def default_source():
    """返回设置页选定的默认相机来源（``front`` 或 ``rear``）。"""
    return device_config.camera_source()


def default_is_rear():
    """默认来源是否为后摄，供需要保留手动切换的应用初始化状态使用。"""
    return default_source() == "rear"


def set_default_source(source):
    """设置全局默认相机来源。通常由系统设置应用调用。"""
    device_config.set_camera_source(source)


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


def read(rear=None, timeout_ms=1000):
    """紧邻读取同侧 ai + rgb 两个通道，返回统一 Frame：

    - model.run(frame) / frame.frame：ai 通道 640x640 原生帧，NPU 零拷贝
    - frame.find_*/draw_*/show / frame.image：rgb 通道 720x480 紧凑 Image

    两路在本函数内连续取得，避免先做几百毫秒推理、再取 RGB 图造成移动
    目标的框与画面错位。不传 rear 时走系统设置的默认摄像头；rear=True
    走后摄 grp3/ch1 + grp3/ch0，rear=False 走前摄 grp0/ch1 + grp0/ch0。

    务必用 `with camera.read() as frame:`——出块会归还 ai 原生帧；
    预装的 RGB Image 是拷贝，到下一次同侧 camera.read()/read_image()
    覆盖 rgb 缓冲前有效。直接调用具体工厂的 Camera.read() 不配对其他
    通道，仍按需把自身帧转成 Image。"""
    if rear is None:
        rear = default_is_rear()
    frame_cam = rear_ai(timeout_ms) if rear else ai(timeout_ms)
    image_cam = rear_rgb(timeout_ms) if rear else rgb(timeout_ms)
    frame = frame_cam.read()
    try:
        frame._image = image_cam.read_image()
        frame._image_camera = image_cam
    except BaseException:
        frame.release()       # RGB 取帧/转换失败，不能把已取的 ai 帧留在池里
        raise
    return frame


def read_image(rear=None, timeout_ms=1000):
    """兼容别名：从 rgb 通道取一帧并直接转成紧凑 RGB Image（720x480），
    原生帧立即归还。不传 rear 时走系统设置的默认摄像头；rear=True 走后摄
    grp3/ch0，rear=False 走前摄 grp0/ch0。

    等价于 (rear_rgb() if rear else rgb()).read_image()。其他通道用
    工厂实例的 cam.read_image()（NV12/NV21 通道出 Y 平面灰度图）；
    转换规则见 Camera._to_image，生命周期见 Frame。"""
    if rear is None:
        rear = default_is_rear()
    cam = rear_rgb(timeout_ms) if rear else rgb(timeout_ms)
    return cam.read_image()


def preview(source=_DEFAULT_SOURCE):
    """屏幕底层相机预览，三态开关：

    - 不传参数：显示设置页选定的默认来源
    - "front"（或 False）：显示前摄 live（grp0/ch2 -> grp1 -> VO）
    - "rear" （或 True） ：显示后摄 live（grp3/ch2 -> grp1 -> VO）
    - "off"  （或 None） ：不显示（视频仍在底层流动以承载 UI 帧，
      仅用不透明底遮住）

    显示态自动把 lv 的 screen_active/layer_bottom 背景透明
    （bg_opa=0），遮挡态置回不透明（bg_opa=255）；screen_load 换屏后
    对新 screen 需重调一次。显示通路未建立时只记录期望（建链时生
    效）；已建立时立即生效，OSD/UI 不受影响。后摄画面要求小核已在
    跑 grp3 采集。"""
    if source is _DEFAULT_SOURCE:
        source = default_source()
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

def rgb(timeout_ms=1000, rear=None) -> Camera:
    """默认来源的 ch0，720x480 BGR888_PLANAR。

    rear 未传时使用系统设置；显式传 True/False 时固定使用后/前摄。
    """
    if rear is None:
        rear = default_is_rear()
    return _get(tdl_py.VpssCamera.rear_rgb if rear else tdl_py.VpssCamera.rgb,
                timeout_ms)

def ai(timeout_ms=1000, rear=None) -> Camera:
    """默认来源的 ch1，640x640 RGB888_PLANAR（NN 推理输入）。"""
    if rear is None:
        rear = default_is_rear()
    return _get(tdl_py.VpssCamera.rear_ai if rear else tdl_py.VpssCamera.ai,
                timeout_ms)

def live(timeout_ms=1000, rear=None) -> Camera:
    """默认来源的 ch2，720x480 NV12（预览同源）。"""
    if rear is None:
        rear = default_is_rear()
    return _get(tdl_py.VpssCamera.rear_live if rear else tdl_py.VpssCamera.live,
                timeout_ms)

def sub_rgb(timeout_ms=1000, rear=None) -> Camera:
    """默认来源的 ch3，320x240 BGR888_PLANAR。"""
    if rear is None:
        rear = default_is_rear()
    return _get(tdl_py.VpssCamera.rear_sub_rgb if rear else tdl_py.VpssCamera.sub_rgb,
                timeout_ms)

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
