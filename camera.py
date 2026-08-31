"""jyd.camera：VPSS 通道取帧。

通道是板级固定配置（dual-OS 小核负责采集），按预设工厂取用：

    前摄采集 grp0 / 显示 grp1：
        rgb()       grp0/ch0  720x480   RGB888_PLANAR（Python 图像处理）
        ai()        grp0/ch1  640x640   RGB888_PLANAR（NN 推理输入，letterbox 黑边）
        live()      grp0/ch2  720x480   NV12（与屏幕预览同源，CV 识别常用）
        sub_rgb()   grp0/ch3  640x640   NV21
        screen()    grp1/ch0  720x480   NV12（显示处理通道，送 VO 的画面）

    后摄采集 grp3（规格为布局名义值，实际以小核配置为准）：
        rear_rgb()  ch0    rear_ai()   ch1    rear_live()  ch2    rear_sub_rgb()  ch3

timeout_ms 仅该通道首次创建生效。首次创建即 open（失败立刻暴露），进程退出自动 close。通道
被其他进程占用（launcher/ai_cycle 在跑）时 open 会失败。

取帧两种形态（模块级便捷函数走 ai / rgb 通道，rear=True 切后摄；
其他通道用工厂实例的 cam.read() / cam.read_image()）：

    with camera.read() as frame:     # ai 通道 zero-copy Frame（NN 推理输入），
        result = model.run(frame)    # 出 with 块即失效，处理必须在块内完成

    img = camera.read_image()        # rgb 通道 RGB Image（剥 stride 填充的
    mks = img.find_qrcodes()         # 拷贝），到下一次 read_image()/read() 前有效

    img = live().read_image()        # NV12/NV21 通道出 Y 平面灰度 Image

屏幕底层预览用 preview() 控制，三态："front" 前摄 / "rear" 后摄 /
"off" 遮挡（显示态自动把 screen/layer_bottom 背景透明，遮挡态置回
不透明）；后摄需小核已在跑 grp3 采集。
"""

import ctypes

import tdl_py

from . import _runtime


class Camera:
    """一个 VPSS 通道。用 rgb()/ai()/live()/screen()/rear_*() 等工厂取实例。"""

    def __init__(self, raw, key):
        self._cam = raw
        self._key = key
        self._group, self._channel = key
        self._held = None      # read_image 的像素缓冲（当前 Image 借用中）

    @property
    def group(self):
        return self._group

    @property
    def channel(self):
        return self._channel

    def read(self):
        """阻塞取一帧（最长 timeout_ms），返回 zero-copy Frame。

        务必用 `with cam.read() as frame:` ——帧引用 VPSS 池内存，
        出块自动归还；下一次 read 也会使上一帧失效。"""
        self._drop_held()
        return self._cam.read()

    def read_image(self):
        """取一帧并按帧格式转成 Image，到下一次同通道
        read_image()/read() 前有效：

        - RGB888 / RGB888_PLANAR：剥行填充、交错成紧凑 RGB Image
          （拷贝约 1 MB，缓冲跨帧复用，帧拷贝后立即归还 VPSS）
        - NV12 / NV21：Y 平面灰度 Image。stride == width 时零拷贝
          （借帧内存，帧由 Camera 持有到下一次读），有行填充时剥成
          紧凑缓冲（拷贝约 0.35 MB）
        - 其他格式抛 RuntimeError"""
        held, self._held = self._held, None    # 旧 Image 按约定此刻失效
        if held is not None and not isinstance(held, bytearray):
            try:
                held.release()                 # 上一次的零拷贝帧，归还 VPSS
            except Exception:
                pass
            held = None
        from . import image
        frame = self._cam.read()
        keep_frame = False
        try:
            w, h = frame.width, frame.height
            n = w * h
            fmt = frame.format
            if fmt in (tdl_py.FORMAT_NV12, tdl_py.FORMAT_NV21):
                stride = frame.strides[0]
                if stride == w:                # 零拷贝：Y 平面直接建灰度视图
                    img = image.new(size=(w, h), mode="L", addr=frame.addr)
                    self._held = frame
                    keep_frame = True
                    return img
                buf = (held if isinstance(held, bytearray) and len(held) == n
                       else bytearray(n))
                mv = frame.data
                for y in range(h):             # 逐行剥掉行尾对齐填充
                    src = y * stride
                    buf[y * w:y * w + w] = mv[src:src + w]
                mode = "L"
            elif fmt in (tdl_py.FORMAT_RGB888, tdl_py.FORMAT_RGB888_PLANAR):
                buf = (held if isinstance(held, bytearray)
                       and len(held) == n * 3 else bytearray(n * 3))
                mv = frame.data
                if fmt == tdl_py.FORMAT_RGB888_PLANAR:
                    tight = bytearray(n)       # 单 plane 去填充的临时缓冲
                    for pi in range(3):
                        base = frame.plane_offsets[pi]
                        stride = frame.strides[pi]
                        if stride == w:
                            tight[:] = mv[base:base + n]
                        else:                  # 逐行剥掉行尾对齐填充
                            for y in range(h):
                                src = base + y * stride
                                tight[y * w:y * w + w] = mv[src:src + w]
                        buf[pi::3] = tight     # plane -> interleaved 分量
                else:                          # FORMAT_RGB888：已是 interleaved
                    row = w * 3
                    stride = frame.strides[0]
                    if stride == row:
                        buf[:] = mv[:row * h]
                    else:
                        for y in range(h):
                            src = y * stride
                            buf[y * row:y * row + row] = mv[src:src + row]
                mode = "RGB"
            else:
                raise RuntimeError(
                    "grp%d/ch%d 帧格式 %d 不支持 read_image（支持 RGB888/"
                    "RGB888_PLANAR/NV12/NV21）"
                    % (self._group, self._channel, fmt))
        finally:
            if not keep_frame:
                frame.release()                # 拷贝路径：帧立即归还 VPSS
        addr = ctypes.addressof((ctypes.c_ubyte * len(buf)).from_buffer(buf))
        img = image.new(size=(w, h), mode=mode, addr=addr)
        self._held = buf
        return img

    def close(self):
        """释放通道（幂等）。一般不用手动调，进程退出自动清理。"""
        self._drop_held()
        if self._cam is not None:
            cam, self._cam = self._cam, None
            _instances.pop(self._key, None)
            cam.close()

    def _drop_held(self):
        if self._held is not None:
            held, self._held = self._held, None
            release = getattr(held, "release", None)   # Frame 需归还，缓冲交 GC
            if release is not None:
                try:
                    release()
                except Exception:
                    pass

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
    """从 ai 通道取一帧，返回 zero-copy Frame（640x640 RGB888_PLANAR，
    NN 推理输入）。rear=True 走后摄 grp3/ch1，否则前摄 grp0/ch1。

    等价于 (rear_ai() if rear else ai()).read()：务必用
    `with camera.read() as frame:` ——出块即失效，推理/处理必须在块内完成。"""
    cam = rear_ai(timeout_ms) if rear else ai(timeout_ms)
    return cam.read()


def read_image(rear=False, timeout_ms=1000):
    """从 rgb 通道取一帧，返回紧凑 interleaved RGB Image（720x480）。
    rear=True 走后摄 grp3/ch0，否则前摄 grp0/ch0。

    等价于 (rear_rgb() if rear else rgb()).read_image()。其他通道用
    工厂实例的 cam.read_image()（NV12/NV21 通道出 Y 平面灰度图），
    转换规则与生命周期见 Camera.read_image。"""
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
    """grp0/ch0，720x480 RGB888_PLANAR。"""
    return _get(tdl_py.VpssCamera.rgb, timeout_ms)

def ai(timeout_ms=1000) -> Camera:
    """grp0/ch1，640x640 RGB888_PLANAR（NN 推理输入）。"""
    return _get(tdl_py.VpssCamera.ai, timeout_ms)

def live(timeout_ms=1000) -> Camera:
    """grp0/ch2，720x480 NV12（预览同源）。"""
    return _get(tdl_py.VpssCamera.live, timeout_ms)

def sub_rgb(timeout_ms=1000) -> Camera:
    """grp0/ch3，640x640 NV21。"""
    return _get(tdl_py.VpssCamera.sub_rgb, timeout_ms)

def screen(timeout_ms=1000) -> Camera:
    """grp1/ch0，720x480 NV12（显示处理通道，读到的是送 VO 的画面）。"""
    return _get(tdl_py.VpssCamera.screen, timeout_ms)


# ---- 后摄 grp3（规格为名义值，实际以小核配置为准） ----

def rear_rgb(timeout_ms=1000) -> Camera:
    """grp3/ch0，720x480 RGB888_PLANAR。"""
    return _get(tdl_py.VpssCamera.rear_rgb, timeout_ms)

def rear_ai(timeout_ms=1000) -> Camera:
    """grp3/ch1，640x640 RGB888_PLANAR（NN 推理输入）。"""
    return _get(tdl_py.VpssCamera.rear_ai, timeout_ms)

def rear_live(timeout_ms=1000) -> Camera:
    """grp3/ch2，720x480 NV12。"""
    return _get(tdl_py.VpssCamera.rear_live, timeout_ms)

def rear_sub_rgb(timeout_ms=1000) -> Camera:
    """grp3/ch3，640x640 NV21。"""
    return _get(tdl_py.VpssCamera.rear_sub_rgb, timeout_ms)
