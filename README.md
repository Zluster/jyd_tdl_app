# dara —— CV184x 板端 Python SDK

面向 CV184x 双系统（大核 Python 3.10）的板端 SDK：VPSS 取帧、MaixPy3 风格
传统视觉、NPU 推理（`.mud` 模型）、嵌入式 LVGL v9 UI。原生依赖
（`tdl_py` / `_maix_image` / `mpy`）捆绑在包内，开箱即用。

- import 即默认启动显示线程（jyd-ui；web 启动时退出按钮立刻可见）；camera/nn/audio 按需初始化，进程退出自动清理
- 与 launcher / ai_cycle 互斥运行（VO/OSD/相机通道独占），跑 dara 脚本前先停掉它们

## 安装

```bash
# 开发机：构建 wheel（在本目录执行，产物在 dist/）
python3 -m pip install build
python3 -m build --wheel --no-isolation

# 板端：二选一
pip install dara-0.1.0-py3-none-any.whl
unzip dara-0.1.0-py3-none-any.whl -d /usr/lib/python3.10/site-packages/
```

注意：wheel 标签是 `py3-none-any`，但包内 `.so` 是 ARM32 原生库，仅供板端安装。

## 快速开始

```python
from dara import camera, lv

label = lv.label(lv.screen_active())
while True:
    img = camera.read_image()            # rgb 通道取图
    mks = img.find_qrcodes()             # 传统视觉：扫二维码
    label.set_text(mks[0]["payload"] if mks else "scanning...")
    lv.show()                            # 可选限速；UI 由内部线程自转
```

## camera —— 取帧

模块级 `read()` 返回的帧同时装着同侧两个通道的数据：**NPU 用 AI 通道，
Image 用 RGB 通道**；`rear=True` 切后摄：

### `camera.read(rear=False, timeout_ms=1000)`

在一次调用里紧邻读取 ai 通道（640×640，RGB888_PLANAR）和 rgb 通道
（720×480，BGR888_PLANAR）。喂 `model.run()` 使用 AI 原生帧，零拷贝、不做
转换；`find_*` / `draw_*` / `lv.show(frame)` 使用已经准备好的 720×480 RGB
Image：

```python
with camera.read() as frame:
    r = model.run(frame)              # AI 640×640 原生帧
    codes = frame.find_qrcodes()      # RGB 720×480 Image
    for box in r.boxes:               # 结果已映射到 720×480，可直接画
        frame.draw_rectangle(box.x1, box.y1, box.x2, box.y2,
                             color=(0, 0, 255), thickness=2)
    lv.show(frame)                    # 显示 720×480 Image
```

`frame.width/height` 属于原生 AI Frame，所以是 640×640；图像尺寸看
`frame.image.width/height`，是 720×480。在帧上画框只改 RGB Image，NPU
看到的仍是原始 AI 帧；直接调 `tdl_py` 底层接口时传 `frame.frame`。

两路在 `camera.read()` 内连续取得，避免先推理几百毫秒、再读取 RGB 图导致移动
物体的框与画面错位。具体通道工厂的 `camera.ai().read()` /
`camera.rgb().read()` 仍只读自己的通道，Image 能力按需由该通道转换。

### `camera.read_image(rear=False, timeout_ms=1000)`

兼容别名：从 rgb 通道（720×480）取一帧并直接转成紧凑 RGB `Image`，原生帧立即
归还，适合纯预览/纯视觉的循环：

```python
img = camera.read_image()
blobs = img.find_blobs([(0, 100, 20, 80, -20, 60)])
```

返回的 `Image` 到下一次 `read_image()`/`read()` 前有效。

## image —— 传统视觉（MaixPy3 风格）

```python
from dara import image
```

`new()` / `load()` / `open()` 及 `Image` 的全部方法（`draw_*` / `find_*` /
`resize` / `crop` / `rotate` / `flip` / `convert` / ...）与 MaixPy3 的 image
模块一致，API 手册见：

<https://wiki.sipeed.com/soft/maixpy3/zh/api/maix/image.html>

dara 扩展：

- `image.show(img)` / `img.show()`：把 Image 同步渲染上屏，就是
  `lv.show(img)` 的便捷写法（见下节）。内部只维护一个 LVGL 图片控件并
  复用，零拷贝共享 img 的像素；返回时这一帧已经上屏，之后覆盖或重画
  img 都安全。支持 L / RGB / RGBA / RGB16；显示画布是 B,G,R 字节序，
  要屏幕红色传 `color=(0, 0, 255)`：

```python
from dara import camera, image

while True:
    img = camera.read_image()
    img.draw_rectangle(100, 100, 300, 260, color=(0, 0, 255), thickness=3)
    image.show(img)              # 或 img.show()
```

- `img.to_lv(parent)` 把 Image 零拷贝显示为 LVGL 控件；等价写法
  `w = lv.image(scr); w.set_src(img)`。Image 像素被控件借用，需保活。

## lv —— 嵌入 LVGL

```python
from dara import lv
```

`lv.*` 即 LVGL v9 API（转发到嵌入解释器；显示通路随 `import dara` 默认
启动，首次访问 lv 时若尚未就绪会等待）。控件、布局、样式等用法参考 LVGL
中文文档：

<https://lvgl.100ask.net/>

dara 特有接口：

- `lv.show(fps=None)`：可选的帧率限速 / 显示 Image。UI 渲染（tick + 触摸）
  由 dara 内部线程自转，不调 show 界面也在跑；纯 UI 循环给 `fps` 防空转。
  传 Image 时同步渲染，返回即已上屏，之后可以放心覆盖或重画这张图
- `lv.bind(obj, lv.EVENT.CLICKED, fn)`：LVGL 事件绑定到 CPython 无参回调
  （回调在 dara 的 UI 线程执行）
- 控件 `set_src(Image)`：直接显示 image 模块的 Image（见上节）

lv 可在任意线程调用（内部转交 UI 线程执行）。

### 屏幕终端（`JYD_LV_USE=0`）

没有终端可看的场景（web 启动、脱机跑脚本）设环境变量 `JYD_LV_USE=0`，
Python 的 `print` / 异常回溯 / `logging` 会同时显示在屏幕正中的深色面板里：
stdout 淡青色、stderr 红色，自动滚到最新一行，原终端或 web 日志照常记录。

```bash
JYD_LV_USE=0 python3 my_script.py
```

只镜像 Python 侧的 `sys.stdout` / `sys.stderr`；原生库直接打印到文件描述符的
内容不会上屏。面板保留最近约 120 段输出，输出洪水时丢最旧并提示丢弃行数。

## nn —— NPU 推理

### `nn.load(spec, threshold=0.5, ...) -> Model`

加载 `.mud` 模型并自动识别模型族（检测 / 分类 / 关键点 / 实例分割 / OCR /
人脸稠密关键点），短名在默认模型目录下解析：

```python
from dara import camera, nn
model = nn.load("yolov8n_det_coco80.mud", threshold=0.25)
```

### `Model.run(frame)`

推理一帧（`frame` 来自 `camera.read()`，调用须在 `with` 块内）。
**结果坐标已从推理帧映射到 720×480 屏幕坐标系**，可直接用于 LVGL 画框：

```python
with camera.read() as frame:
    r = model.run(frame)
for box in r.boxes:
    print(r.label_of(box.class_id), box.x1, box.y1, box.x2, box.y2)
```

### `Model.reset()`

显式卸载模型、释放 NPU 内存（可选；进程退出时系统兜底回收）。
