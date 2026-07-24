# tdl_py：VPSS 零拷取帧与 OSD 使用指南

本文档对应 `python/tdl_py_module.cpp` 当前实现，适用于 CV184X 双系统场景：

- 小核负责 VI、VPSS、VO、RGN 等媒体模块的初始化和运行。
- 大核 Python 3.10 通过 `tdl_py` 调用 MMF 接口。
- VPSS 帧使用 `CVI_SYS_Mmap` 映射为只读 `memoryview`，不复制像素。
- OSD 画布以可写 `memoryview` 或整数地址暴露，可供 Python、ctypes、LVGL 等直接写入。

本文所说的“零拷贝”是指 Python 直接访问映射后的媒体缓冲区，不额外复制整帧像素。调用 `bytes(memoryview)`、切片产生 `bytes`、颜色转换等操作仍会产生复制。

---

## 1. 模块能力概览

`tdl_py` 主要提供以下对象：

- `VpssCamera`：从指定 VPSS group/channel 阻塞取帧。
- `Frame`：一帧 VPSS 图像及其零拷贝只读视图。
- `VoOutput`：打开或关闭 MIPI/LVDS 等 VO 输出。
- `MediaLink`：建立 VPSS→VPSS、VPSS→VO 的硬件绑定。
- `Osd`：创建 RGN Overlay、绑定到 VPSS 通道并提交画布。
- `OsdCanvas`：OSD 画布描述，包括地址、大小、stride 和可写 `memoryview`。

模块还提供：

- 像素格式常量，例如 `FORMAT_NV12`、`FORMAT_NV21`、`FORMAT_ARGB8888`。
- 双系统布局常量，例如 `CAPTURE_GROUP`、`DISPLAY_GROUP`、`LIVE_CHANNEL`。
- VO 接口和时序常量。
- 查询现有 VO、VPSS/VO bind 状态的函数。

---

## 2. CV184X 双系统默认媒体布局

当前模块内置以下 VPSS 预设：

| Python 构造方法 | VPSS 通道 | 默认规格 | 像素格式 |
|---|---:|---:|---|
| `VpssCamera.main()` | grp0/ch0 | 1920×1080 | NV12 |
| `VpssCamera.ai()` | grp0/ch1 | 640×640 | RGB888_PLANAR |
| `VpssCamera.live()` | grp0/ch2 | 1280×720 | NV12 |
| `VpssCamera.sub_rgb()` | grp0/ch3 | 640×640 | NV21 |
| `VpssCamera.screen()` | grp1/ch0 | 1280×720 | NV12 |

显示链路通常为：

```text
摄像头/小核媒体管线
        │
        ▼
VPSS grp0/ch2（live，1280×720）
        │ CVI_SYS_Bind
        ▼
VPSS grp1/ch0（显示处理通道）
        │ CVI_SYS_Bind
        ▼
VO layer0/ch0
        │ 旋转 90°
        ▼
MIPI 720×1280 竖屏
```

OSD 可以绑定到：

- `grp1/ch0`：只影响显示路径，适合 UI、检测框、状态栏。
- `grp0/ch2`：在 live 源通道上叠加，后续显示、编码等下游都可能带上该 OSD。

OSD 本身不会产生视频帧。目标 VPSS 通道没有帧流动时，即使 `create()`、`attach()`、`update()` 都成功，屏幕上也不会出现内容。

---

## 3. 初始化和导入

```python
import tdl_py

tdl_py.init()
```

通常不必手动调用 `init()`：

- `VpssCamera.open()` 会自动初始化 MMF runtime。
- `Osd.create()` 会自动初始化 MMF runtime。
- `VoOutput.open()` 和 `MediaLink.bind()` 也会自动初始化。

初始化底层会调用 `CVI_SYS_Init()`，并连接小核的 `CVI_MMF_MSG` 服务。

---

## 4. VPSS 零拷取帧

### 4.1 最简单的取帧方式

```python
import tdl_py

cam = tdl_py.VpssCamera.live(timeout_ms=1000)

with cam.read() as frame:
    print("size:", frame.width, frame.height)
    print("format:", frame.format)
    print("planes:", frame.plane_count)
    print("strides:", frame.strides)
    print("plane sizes:", frame.plane_sizes)
    print("virtual addr:", hex(frame.addr))
    print("physical addr:", hex(frame.phys_addr))
    print("mapped bytes:", frame.size)
    print("sequence:", frame.sequence)
    print("timestamp_us:", frame.timestamp_us)

cam.close()
```

`read()` 在等待 VPSS 帧时会释放 Python GIL，因此不会因为阻塞取帧而完全阻塞其他 Python 线程。

### 4.2 Frame 属性

- `width`、`height`：有效图像宽高。
- `format`：底层像素格式编号。
- `sequence`：帧序号，对应底层 `u32TimeRef`。
- `timestamp_us`：时间戳，对应底层 `u64PTS`。
- `phys_addr`：第 0 plane 的物理地址。
- `addr`：`CVI_SYS_Mmap` 后的虚拟地址，整数形式。
- `size`：所有 plane 映射区域的总字节数。
- `plane_count`：有效 plane 数量。
- `strides`：3 个 plane 的行跨度，单位为字节。
- `plane_sizes`：3 个 plane 的底层长度。
- `plane_offsets`：各 plane 相对 `addr` 的字节偏移。
- `valid`：当前映射是否仍然有效。
- `data`：覆盖所有 plane 的只读零拷贝 `memoryview`。
- `plane(i)`：指定 plane 的只读零拷贝 `memoryview`。

### 4.3 NV12 示例

1280×720 NV12 通常表现为：

```text
plane 0: Y，  stride=1280，size=1280×720=921600
plane 1: UV， stride=1280，size=1280×360=460800
总大小：1382400
```

访问两个 plane：

```python
with tdl_py.VpssCamera.live().read() as frame:
    y = frame.plane(0)
    uv = frame.plane(1)
    print(len(y), len(uv))
```

不要假设所有分辨率都满足 `stride == width`。处理每一行时应使用 `frame.strides[i]`，而不是只用图像宽度推算下一行地址。

### 4.4 RGB888_PLANAR 示例

`VpssCamera.ai()` 返回 RGB planar 数据，通常有 3 个 plane：

```python
with tdl_py.VpssCamera.ai().read() as frame:
    r = frame.plane(0)
    g = frame.plane(1)
    b = frame.plane(2)
```

实际 plane 含义应以小核 VPSS 配置及 `frame.format` 为准。

### 4.5 使用整数地址

CPython 下可通过 ctypes 包装：

```python
import ctypes
import tdl_py

with tdl_py.VpssCamera.live().read() as frame:
    array_type = ctypes.c_ubyte * frame.size
    buf = array_type.from_address(frame.addr)
    print(buf[0])
```

这仍然是零拷贝。`buf` 并不拥有媒体内存，Frame 失效后不得继续访问。

如果需要真正的 Python `bytes`：

```python
with tdl_py.VpssCamera.live().read() as frame:
    copied = bytes(frame.data)
```

这里会复制整帧，不再是零拷贝。

### 4.6 Frame 生命周期：最重要的约束

每个 `VpssCamera` 同时只允许一帧处于有效状态。

以下任一操作都会使已有 Frame 的映射失效：

- `frame.release()`。
- 退出 `with cam.read() as frame:`。
- 同一个 camera 再次调用 `cam.read()`。
- 调用 `cam.close()`。

正确：

```python
cam = tdl_py.VpssCamera.live()
with cam.read() as frame:
    consume(frame.data)
cam.close()
```

错误：

```python
cam = tdl_py.VpssCamera.live()
frame1 = cam.read()
view1 = frame1.data
frame2 = cam.read()       # frame1/view1 此时已经失效
print(view1[0])           # 禁止：可能访问已解除映射的内存
```

绑定层能够阻止释放后再次获取 `frame.addr`、`frame.data` 或 `frame.plane()`，但已经提前保存到其他变量里的旧 `memoryview` 或 ctypes 对象无法被自动撤销。访问旧对象属于未定义行为，严重时会导致 Python 进程段错误。

---

## 5. 建立完整显示链路

仅创建 OSD 不足以点亮屏幕。完整显示通常需要：

1. 打开 VO。
2. 将 grp0/ch2 绑定到 grp1/ch0。
3. 将 grp1/ch0 绑定到 VO。
4. 创建并 attach OSD。

### 5.1 基本流程

```python
import tdl_py

vo = tdl_py.VoOutput()
vo.open()

preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
preview.bind()

display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
display.bind()
```

默认 `VoOutput()` 配置为：

- VO device 0。
- layer 0。
- channel 0。
- 输出尺寸 720×1280。
- NV12。
- MIPI。
- `P720_1280_60`。
- 硬件旋转 90°。

VPSS/OSD 画布仍按 1280×720 横屏坐标绘制，VO 会把最终视频和 OSD 一起旋转到 720×1280 竖屏。

### 5.2 避免重复打开和重复 bind

```python
vo = None
preview = None
display = None

if not tdl_py.vo_is_enabled(0):
    vo = tdl_py.VoOutput()
    vo.open()

if tdl_py.get_bind_source_vpss(1, 0) is None:
    preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
    preview.bind()

if tdl_py.get_bind_source_vo(0, 0) is None:
    display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
    display.bind()
```

查询函数返回：

- `None`：目标当前没有 bind source。
- `("vpss", device, channel)` 等三元组：目标已经由该源供帧。

不能只判断“非 None”就认定链路一定正确，还应在重要业务中核对返回值是否是预期源：

```python
source = tdl_py.get_bind_source_vpss(1, 0)
if source not in (None, ("vpss", 0, 2)):
    raise RuntimeError("grp1/ch0 已绑定到意外来源: %r" % (source,))
```

### 5.3 清理顺序

按建立顺序的反方向清理：

```python
if display is not None:
    display.unbind()
if preview is not None:
    preview.unbind()
if vo is not None:
    vo.close()
```

只清理由当前进程创建的对象。若启动时发现链路已经存在并选择复用，就不要在退出时拆掉其他进程创建的链路。

---

## 6. OSD 基础概念

### 6.1 创建参数

```python
osd = tdl_py.Osd(
    handle=201,
    width=1280,
    height=720,
    pixel_format=tdl_py.FORMAT_ARGB8888,
    canvas_count=2,
    bg_color=0,
)
```

参数含义：

- `handle`：RGN 全局编号，不是系统返回值。不同 OSD 必须使用不同 handle，并避免与小核或其他进程正在使用的 handle 冲突。
- `width`、`height`：OSD 画布尺寸。
- `pixel_format`：默认 ARGB8888。
- `canvas_count`：物理画布数量，当前重点支持 1 或 2。
- `bg_color`：底层区域背景色。

`handle` 只负责标识区域；位置由 `attach(x, y)` 决定，叠加顺序由 `layer` 决定。

### 6.2 创建和绑定

```python
osd.create()
osd.attach(group=1, channel=0, x=0, y=0, layer=10)
```

`attach()` 默认参数就是 grp1/ch0、原点、layer 10：

```python
osd.attach()
```

同一个 `Osd` 对象已经 attach 后再次调用 `attach()`，底层 `OsdRegion` 会直接返回成功，不会自动切换到另一个通道。若要换目标，应先 `detach()` 再 attach。

### 6.3 显示、隐藏、移动

```python
osd.set_visible(True)
osd.set_visible(False)
osd.move_to(100, 50)
```

- `set_visible()` 只改变区域显示开关，不销毁内容。
- `move_to()` 改变 OSD 在目标 VPSS 通道上的左上角坐标。
- `layer` 只决定挂在同一通道上的多个区域之间的层级。

### 6.4 ARGB8888 的内存字节序

在当前小端 ARM 平台上，ARGB8888 的内存字节顺序按每像素 4 字节观察通常为：

```text
B, G, R, A
```

例如：

```python
blue_78_percent = bytes([255, 0, 0, 200])
red_78_percent  = bytes([0, 0, 255, 200])
transparent     = bytes([0, 0, 0, 0])
```

必须以实际平台和 RGN 格式验证结果。若外部图像库导出 RGBA（R、G、B、A），通常需要交换 R/B 才能写入当前 OSD。

### 6.5 stride 和 size

`OsdCanvas` 属性：

- `addr`：画布虚拟地址，整数。
- `size`：`stride × height`。
- `width`、`height`：画布像素尺寸。
- `stride`：每行实际字节数。
- `format`：像素格式。
- `data`：覆盖整个画布的可写零拷贝 `memoryview`。

不要假设 `stride == width × 4`。画矩形或复制图像时必须逐行按 stride 定位：

```python
def fill_rect_bgra(canvas, x, y, width, height, color):
    pixel = bytes(color)
    row = pixel * width
    for dy in range(height):
        begin = (y + dy) * canvas.stride + x * 4
        canvas.data[begin:begin + width * 4] = row
```

---

## 7. 三种 OSD 画布模式

当前绑定提供三种用法：

1. `canvas()`：普通 SDK 画布，每轮重新获取。
2. `persistent_canvas()`：固定地址单缓冲。
3. `persistent_pair()`：固定地址双缓冲。

三种模式不要在同一个 `Osd` 对象上混用。

---

## 8. 普通 canvas() 模式

### 8.1 标准绘制循环

```python
osd = tdl_py.Osd(handle=201, canvas_count=2)
osd.create()
osd.attach()

while running:
    c = osd.canvas()

    # 建议每帧先清透明，再完整绘制。
    c.data[:] = b"\x00" * c.size
    fill_rect_bgra(c, 20, 20, 200, 80, (0, 0, 255, 200))

    osd.update()
```

严格顺序为：

```text
canvas() / CVI_RGN_GetCanvasInfo
        ↓
写当前 back buffer
        ↓
update() / CVI_RGN_UpdateCanvas
        ↓
该 buffer 被提交，下一轮重新 canvas()
```

### 8.2 地址有效期

普通 `canvas()` 返回的是 RGN SDK 当前 back buffer 的视图：

- 只保证在与它配对的 `update()` 之前有效。
- `update()` 后不要继续使用该 `OsdCanvas.data` 或 `addr`。
- 下一轮必须重新调用 `canvas()`。
- 双缓冲下下一轮地址通常在两个地址之间交替。
- 不应自行缓存地址并猜测下一轮该写哪一块。

即使单缓冲时多次观察到地址相同，普通 `canvas()` API 仍应遵循“每轮重新获取”的协议。

### 8.3 普通双缓冲的特点

`canvas_count=2` 时：

- 一个 buffer 由硬件用于显示/叠加。
- 另一个 buffer 提供给应用绘制。
- `update()` 后前后台交换。
- 应用不容易写到当前显示 buffer，撕裂风险更低。

但是两块缓冲的内容彼此独立。若只改一小块脏区域：

- 第 1 帧修改 buffer A。
- 第 2 帧拿到 buffer B，B 不会自动包含 A 的最新内容。
- 第 3 帧又回到 A。

因此普通双缓冲最安全的方式是每轮完整重画，或者由应用分别维护两块画布的一致状态。

### 8.4 适用场景

- 普通 Python 绘制。
- 每帧完整清屏、重画。
- 不需要把固定地址长期交给外部渲染器。
- 希望遵循最保守的 RGN SDK 使用方式。

---

## 9. persistent_canvas()：固定地址单缓冲

### 9.1 设计目的

`persistent_canvas()` 用于外部渲染器需要一个长期稳定 framebuffer 地址的场景，例如：

- LVGL DIRECT 模式。
- C/C++ 库长期持有 framebuffer 指针。
- ctypes/FFI 需要固定地址。

它要求创建 OSD 时使用：

```python
canvas_count=1
```

### 9.2 与普通 canvas() 的区别

普通 `canvas()` 返回 SDK 提供的当前虚拟地址；`persistent_canvas()` 会：

1. 调用 `GetCanvasInfo` 取得当前画布物理地址。
2. 使用 `CVI_SYS_Mmap` 建立由 `PyOsd` 自己持有的映射。
3. 把整块画布清为透明，避免首帧垃圾。
4. 返回该自有映射的固定虚拟地址。
5. 在 `destroy()` 或对象析构时 `CVI_SYS_Munmap`。

因此：

- `persistent_canvas().addr` 在整个 OSD 生命周期内固定。
- 对应 `data` 可跨多次 `update()` 使用。
- 地址不是普通 `canvas()` 返回地址的生命周期语义。
- `destroy()` 后地址和 memoryview 立即失效。

### 9.3 基本示例

```python
osd = tdl_py.Osd(handle=201, canvas_count=1)
osd.create()
osd.attach()

fb = osd.persistent_canvas()
print(hex(fb.addr), fb.size, fb.stride)

fb.data[:] = b"\x00" * fb.size
fill_rect_bgra(fb, 20, 20, 200, 80, (0, 0, 255, 200))
osd.update()

# 地址和 memoryview 保持有效，可以继续写同一块内存。
fill_rect_bgra(fb, 300, 20, 200, 80, (255, 0, 0, 200))
osd.update()
```

### 9.4 update() 如何维持 SDK 协议

RGN 要求 `GetCanvasInfo` 与 `UpdateCanvas` 严格交替。persistent 模式下用户不再每轮调用 `canvas()`，因此绑定层会在需要时自动重新执行 `GetCanvasInfo` 以“arm”下一次 update，并验证物理地址仍然属于已映射的固定画布。

单缓冲模式下，每次重新获取都应返回相同物理地址。若物理地址变化，`update()` 会抛出异常：

```text
osd canvas physical address changed; persistent canvases require a fixed canvas ring
```

### 9.5 单缓冲优缺点

优点：

- 固定地址。
- 只需一个 framebuffer。
- 最适合 LVGL 单 framebuffer DIRECT 渲染。
- 增量绘制自然成立，因为始终操作同一块内容。
- 内存占用比双缓冲小。

缺点：

- CPU/LVGL 写入时，硬件可能同时读取该 buffer。
- 快速、大面积更新理论上可能撕裂。
- `update()` 是提交/通知，不会神奇地把单缓冲变成无撕裂双缓冲。

### 9.6 LVGL 单缓冲建议

前提：

```python
fb.stride == width * 4
```

如果 stride 有 padding，LVGL 直接 framebuffer 模式可能无法正确处理，需使用 PARTIAL 模式并在 flush 回调中逐行拷贝。

概念示例：

```python
osd = tdl_py.Osd(
    handle=201,
    width=1280,
    height=720,
    canvas_count=1,
)
osd.create()
osd.attach()

fb = osd.persistent_canvas()
assert fb.stride == fb.width * 4

disp = lv.display_create(fb.width, fb.height)
disp.set_color_format(lv.COLOR_FORMAT.ARGB8888)
disp.set_buffers(fb.data, None, fb.size, lv.DISPLAY_RENDER_MODE.DIRECT)

def flush_cb(display, area, px_map):
    if display.flush_is_last():
        osd.update()
    display.flush_ready()

disp.set_flush_cb(flush_cb)
```

具体 LVGL Python 绑定的函数名可能不同，应以当前绑定版本为准。关键原则是：

- framebuffer 就是 `fb.data` 或 `fb.addr`。
- flush 回调不复制像素。
- 一轮刷新最后一次 flush 时调用 `osd.update()`。
- 无论 update 成功还是异常，都要按 LVGL 要求处理 flush 状态，避免刷新线程永久卡住。

---

## 10. persistent_pair()：固定地址双缓冲

### 10.1 设计目的

`persistent_pair()` 用于：

- 外部渲染器需要两个长期固定的 framebuffer 地址。
- 希望外部渲染器的 buffer 交替与 RGN 的双缓冲翻转保持同相位。
- LVGL DIRECT 双缓冲。

必须创建：

```python
canvas_count=2
```

### 10.2 初始化时发生什么

第一次调用 `persistent_pair()` 时，绑定层执行：

1. 获取当前 back buffer A。
2. 对 A 建立持久映射并清透明。
3. 调用一次 `update()`，使 A 成为 visible/front。
4. 获取新的 back buffer B。
5. 对 B 建立持久映射并清透明。
6. 验证 A、B 物理地址不同。
7. 返回 `(back, front)`，也就是 `(B, A)`。

因此该方法在初始化期间会主动提交一帧透明画面。这是为了确定两个 buffer 的相位。

### 10.3 基本示例

```python
osd = tdl_py.Osd(handle=201, canvas_count=2)
osd.create()
osd.attach()

back, front = osd.persistent_pair()
print("first writable back:", hex(back.addr))
print("current front:", hex(front.addr))

# 第一帧必须写 back。
back.data[:] = b"\x00" * back.size
fill_rect_bgra(back, 20, 20, 200, 80, (0, 0, 255, 200))
osd.update()

# 下一帧写另一块，即初始化时返回的 front。
front.data[:] = b"\x00" * front.size
fill_rect_bgra(front, 300, 20, 200, 80, (255, 0, 0, 200))
osd.update()
```

如果应用自己管理交替，顺序必须是：

```python
buffers = osd.persistent_pair()   # (B, A)
index = 0

while running:
    fb = buffers[index]
    render_full_frame(fb)
    osd.update()
    index ^= 1
```

任何一次漏写、额外 update、update 失败后仍然切换 index，都可能造成应用 buffer 相位与 RGN 相位错位。

### 10.4 与 LVGL 双缓冲配合

当前绑定明确约定：

```python
buf1, buf2 = osd.persistent_pair()
```

返回值已经按 `(next back, current front)` 排序，目标是把它们按 `(buf1, buf2)` 交给 LVGL DIRECT 模式，使 LVGL 的交替顺序与 RGN 每次 `update()` 的交换顺序保持一致。

概念示例：

```python
osd = tdl_py.Osd(handle=201, canvas_count=2)
osd.create()
osd.attach()

buf1, buf2 = osd.persistent_pair()
assert buf1.stride == buf1.width * 4
assert buf2.stride == buf2.width * 4

disp = lv.display_create(buf1.width, buf1.height)
disp.set_color_format(lv.COLOR_FORMAT.ARGB8888)
disp.set_buffers(
    buf1.data,
    buf2.data,
    buf1.size,
    lv.DISPLAY_RENDER_MODE.DIRECT,
)

def flush_cb(display, area, px_map):
    if display.flush_is_last():
        osd.update()
    display.flush_ready()

disp.set_flush_cb(flush_cb)
```

必须验证当前 LVGL Python 绑定在 DIRECT 双缓冲模式下：

- 第一帧确实从 `buf1` 开始渲染。
- 每个完整刷新周期只触发一次 RGN `update()`。
- LVGL 的 buffer 切换时点与 `flush_is_last()` 相符。

如果 LVGL 版本或 Python binding 的首帧 buffer 顺序不同，可能导致相位错位。应先用两块明显不同的纯色画面做验证。

### 10.5 固定双缓冲优缺点

优点：

- 两个地址在 OSD 生命周期内固定。
- 外部渲染器可直接持有两个 framebuffer。
- 正确同步时可降低撕裂。
- 适合全帧 DIRECT 渲染。

缺点：

- 相位管理比单缓冲严格。
- 初始化会提交一帧透明内容。
- 任何不配对的 update 都可能破坏交替顺序。
- 两块 buffer 内容独立；增量渲染必须确保渲染器维护两块内容一致性。
- 内存占用约为单缓冲两倍。

---

## 11. 三种画布模式如何选择

### 11.1 选择普通 canvas()

适合：

- 普通 Python 程序。
- 每帧重新取地址可以接受。
- 每帧完整重画。
- 更看重 SDK 标准使用方式和简单可靠。

推荐配置：

```python
canvas_count=2
```

### 11.2 选择 persistent_canvas()

适合：

- LVGL 单 framebuffer。
- 外部模块要求固定地址。
- 增量 UI 更新。
- 可以接受潜在撕裂。

必须配置：

```python
canvas_count=1
```

### 11.3 选择 persistent_pair()

适合：

- LVGL 或其他渲染器明确支持两个 DIRECT framebuffer。
- 能保证每个完整刷新周期恰好调用一次 `update()`。
- 能验证并维护 buffer 相位。

必须配置：

```python
canvas_count=2
```

### 11.4 不要这样做

- `canvas_count=2` 却长期缓存普通 `canvas().addr`。
- `persistent_canvas()` 后又调用普通 `canvas()`。
- `persistent_pair()` 后自行额外调用 `canvas()`。
- 获取画布后不 update，又再次获取画布。
- update 两次但中间没有对应的画布获取/重 arm。
- `destroy()` 后继续使用旧 memoryview 或地址。
- 把 `(back, front)` 顺序随意反转后直接交给渲染器。

---

## 12. 在两个 VPSS 通道上创建两个 OSD

以下例子在：

- grp0/ch2 上画顶部红条。
- grp1/ch0 上画底部蓝条。

```python
import time
import tdl_py

vo = tdl_py.VoOutput()
vo.open()

preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
preview.bind()

display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
display.bind()

osd_live = tdl_py.Osd(handle=201, canvas_count=2)
osd_live.create()
osd_live.attach(group=0, channel=2, layer=1)
c_live = osd_live.canvas()
c_live.data[:] = b"\x00" * c_live.size
c_live.data[:c_live.stride * 100] = (
    bytes([0, 0, 255, 200]) * (c_live.stride * 100 // 4)
)
osd_live.update()

osd_screen = tdl_py.Osd(handle=202, canvas_count=2)
osd_screen.create()
osd_screen.attach(group=1, channel=0, layer=1)
c_screen = osd_screen.canvas()
c_screen.data[:] = b"\x00" * c_screen.size
c_screen.data[c_screen.stride * 620:c_screen.stride * 720] = (
    bytes([255, 0, 0, 200]) * (c_screen.stride * 100 // 4)
)
osd_screen.update()

osd_live.set_visible(True)
osd_screen.set_visible(True)

time.sleep(10)

osd_screen.destroy()
osd_live.destroy()
display.unbind()
preview.unbind()
vo.close()
```

预期：

- 屏幕可同时看到两个 OSD。
- grp0/ch2 上的 OSD 位于显示链路上游。
- grp1/ch0 上的 OSD 只位于显示处理通道。

是否能从 `VpssCamera.live()` 读取到 grp0/ch2 的 RGN 合成结果，还取决于 CVITEK RGN/VPSS 的具体叠加时点和该通道输出路径，应以上板实际验证为准，不能仅凭 attach 位置假定内存取帧一定包含 OSD。

---

## 13. 把外部图像复制到 OSD

假设外部图像是紧密排列的 BGRA：

```python
def blit_bgra(canvas, src, src_width, src_height, x=0, y=0):
    copy_width = min(src_width, canvas.width - x)
    copy_height = min(src_height, canvas.height - y)
    if copy_width <= 0 or copy_height <= 0:
        return

    src_stride = src_width * 4
    row_bytes = copy_width * 4
    for row in range(copy_height):
        src_begin = row * src_stride
        dst_begin = (y + row) * canvas.stride + x * 4
        canvas.data[dst_begin:dst_begin + row_bytes] = \
            src[src_begin:src_begin + row_bytes]
```

若源图是 RGBA，则先交换 R/B，或者在生成源图时直接输出 BGRA。转换通常会产生额外内存和复制，不属于零拷贝。

---

## 14. OSD 生命周期和资源释放

推荐顺序：

```text
Osd(...)
  ↓
create()
  ↓
attach()
  ↓
获取并写画布
  ↓
update()
  ↓
set_visible() / move_to() / 重复绘制
  ↓
destroy()
```

`destroy()` 会：

1. 解除 persistent 映射。
2. detach RGN。
3. destroy RGN。

对象析构时：

- `PyOsd` 先解除它自己建立的 persistent mmap。
- `OsdRegion` 析构函数再 detach 和 destroy。

因此最好显式调用 `destroy()`，不要依赖 Python GC 的不确定时机。

OSD 销毁后以下对象全部失效：

- 普通 `OsdCanvas`。
- persistent 单缓冲视图。
- persistent 双缓冲的两个视图。
- 由这些地址创建的 ctypes、uctypes 或外部 C 指针对象。

---

## 15. 异常安全的完整结构

```python
import tdl_py

vo = None
preview = None
display = None
osd = None

try:
    if not tdl_py.vo_is_enabled(0):
        vo = tdl_py.VoOutput()
        vo.open()

    source = tdl_py.get_bind_source_vpss(1, 0)
    if source is None:
        preview = tdl_py.MediaLink.vpss_to_vpss(0, 2, 1, 0)
        preview.bind()
    elif source != ("vpss", 0, 2):
        raise RuntimeError("grp1/ch0 来源异常: %r" % (source,))

    source = tdl_py.get_bind_source_vo(0, 0)
    if source is None:
        display = tdl_py.MediaLink.vpss_to_vo(1, 0, 0, 0)
        display.bind()
    elif source != ("vpss", 1, 0):
        raise RuntimeError("VO layer0/ch0 来源异常: %r" % (source,))

    osd = tdl_py.Osd(handle=201, canvas_count=2)
    osd.create()
    osd.attach(group=1, channel=0, layer=10)

    c = osd.canvas()
    c.data[:] = b"\x00" * c.size
    fill_rect_bgra(c, 20, 20, 200, 80, (0, 0, 255, 200))
    osd.update()
    osd.set_visible(True)

    input("按 Enter 退出...")

finally:
    if osd is not None:
        osd.destroy()
    if display is not None:
        display.unbind()
    if preview is not None:
        preview.unbind()
    if vo is not None:
        vo.close()
```

生产程序还应处理 `SIGINT`、`SIGTERM`，确保退出时执行清理，避免残留 bind 或 RGN handle。

---

## 16. 常见问题

### 16.1 OSD API 都成功，但屏幕是黑的

检查：

- 小核 MMF 服务是否正常。
- VO 是否打开。
- grp0/ch2 是否绑定到 grp1/ch0。
- grp1/ch0 是否绑定到 VO。
- 目标 VPSS 通道是否持续有帧。

OSD 不是独立 framebuffer 输出，它依附于视频通道。

### 16.2 create() 失败

常见原因：

- handle 已被其他进程或小核占用。
- 上次程序异常退出，区域未正确清理。
- 尺寸或像素格式不被当前 RGN 配置支持。

更换 handle 只能用于诊断；生产上应建立明确的 handle 分配策略。

### 16.3 bind() 失败

先查询：

```python
print(tdl_py.get_bind_source_vpss(1, 0))
print(tdl_py.get_bind_source_vo(0, 0))
```

目标可能已经绑定。不要盲目重复 bind，也不要随意 unbind 不属于当前进程的链路。

### 16.4 OSD 颜色红蓝颠倒

确认源数据是 RGBA 还是 BGRA。当前 ARGB8888 在内存中通常按 B、G、R、A 访问。

### 16.5 画面斜行或错位

通常是忽略 stride。每行起点必须用：

```python
row_start = y * canvas.stride
```

而不是：

```python
row_start = y * canvas.width * 4
```

只有确认二者相等后才能简化。

### 16.6 双缓冲出现隔帧残影

两块画布内容不同，而应用只更新了局部区域。解决办法：

- 每帧完整清屏并重画。
- 在两块 buffer 上同步执行相同的增量修改。
- 改用固定单缓冲。
- 让 LVGL 等渲染器正确维护双 framebuffer 内容。

### 16.7 persistent_canvas() 第二次 update 报物理地址变化

通常是创建时用了 `canvas_count=2`。固定单缓冲必须：

```python
tdl_py.Osd(..., canvas_count=1)
```

### 16.8 persistent_pair() 返回相同地址

通常说明底层没有创建两个独立物理画布，或配置与预期不符。必须使用 `canvas_count=2`。

### 16.9 Python 偶发段错误

重点排查：

- Frame release/下一次 read 后仍访问旧 frame memoryview。
- OSD destroy 后仍访问旧 canvas。
- 普通 canvas update 后仍缓存并写旧地址。
- 外部 C/LVGL 对象持有的地址生命周期超过 `Osd`。

这类错误无法靠 Python 异常完全保护，因为地址已经交给了外部代码。

---

## 17. 性能与线程建议

- VPSS `Frame.data`、`Frame.plane()` 本身不复制。
- OSD `OsdCanvas.data` 本身不复制。
- `bytes(...)`、大块 `b"\x00" * size`、图像格式转换会分配和复制。
- 高频全屏清透明可考虑由 C/LVGL 直接写固定画布，减少 Python 临时对象。
- 不要让多个线程同时写同一 OSD buffer。
- 不要让一个线程 `update()`，另一个线程仍在写该 back buffer。
- persistent 双缓冲必须由一个明确的渲染/提交线程维护相位。
- VPSS camera 每次只能保留一个有效帧；需要跨线程处理时，应在 release 前完成，或复制到自有内存。

---

## 18. API 快速参考

### 18.1 VpssCamera

```python
tdl_py.VpssCamera(group, channel, timeout_ms=1000)
tdl_py.VpssCamera.main(timeout_ms=1000)
tdl_py.VpssCamera.ai(timeout_ms=1000)
tdl_py.VpssCamera.live(timeout_ms=1000)
tdl_py.VpssCamera.sub_rgb(timeout_ms=1000)
tdl_py.VpssCamera.screen(timeout_ms=1000)

camera.open()
frame = camera.read()
camera.close()
```

### 18.2 Frame

```python
frame.width
frame.height
frame.format
frame.sequence
frame.timestamp_us
frame.phys_addr
frame.plane_count
frame.valid
frame.addr
frame.size
frame.data
frame.strides
frame.plane_sizes
frame.plane_offsets
frame.plane(index)
frame.release()
```

### 18.3 Osd

```python
osd = tdl_py.Osd(
    handle,
    width=1280,
    height=720,
    pixel_format=tdl_py.FORMAT_ARGB8888,
    canvas_count=2,
    bg_color=0,
)

osd.create()
osd.attach(group=1, channel=0, x=0, y=0, layer=10)
canvas = osd.canvas()
canvas = osd.persistent_canvas()
back, front = osd.persistent_pair()
osd.update()
osd.set_visible(True)
osd.move_to(x, y)
osd.detach()
osd.destroy()
```

### 18.4 OsdCanvas

```python
canvas.addr
canvas.size
canvas.width
canvas.height
canvas.stride
canvas.format
canvas.data
```

### 18.5 VoOutput

```python
vo = tdl_py.VoOutput(
    device=0,
    layer=0,
    channel=0,
    width=720,
    height=1280,
    pixel_format=tdl_py.FORMAT_NV12,
    interface_type=tdl_py.INTERFACE_MIPI,
    interface_sync=tdl_py.SYNC_720x1280_60,
    rotation=90,
)

vo.open()
vo.opened
vo.close()
```

### 18.6 MediaLink

```python
link = tdl_py.MediaLink.vpss_to_vpss(
    src_group, src_channel, dst_group, dst_channel
)

link = tdl_py.MediaLink.vpss_to_vo(
    src_group, src_channel, layer=0, channel=0
)

link.bind()
link.bound
link.unbind()
```

### 18.7 状态查询

```python
tdl_py.vo_is_enabled(device=0)
tdl_py.get_bind_source_vpss(group, channel=0)
tdl_py.get_bind_source_vo(layer=0, channel=0)
```

---

## 19. 最终建议

普通 Python OSD：

```text
canvas_count=2 + 每轮 canvas() + 完整重画 + update()
```

LVGL 固定单 framebuffer：

```text
canvas_count=1 + persistent_canvas() + DIRECT + 每完整刷新一次 update()
```

LVGL 固定双 framebuffer：

```text
canvas_count=2 + persistent_pair() + DIRECT + 严格维护首帧和交替相位
```

不确定时优先从普通 `canvas_count=2` 模式验证显示链路、尺寸、颜色、stride 和 OSD attach；确认基础路径正确后，再切换 persistent 模式接入 LVGL。
