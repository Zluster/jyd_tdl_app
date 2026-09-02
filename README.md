# dara —— CV184x 板端 Python SDK

面向 CV184x 双系统（大核 Python 3.10）的板端 SDK：VPSS 取帧、MaixPy3 风格
传统视觉、NPU 推理（`.mud` 模型）、嵌入式 LVGL v9 UI。原生依赖
（`tdl_py` / `_maix_image` / `mpy`）捆绑在包内，开箱即用。

- import 不碰硬件：首次用到哪块才初始化哪块，进程退出自动清理
- 与 launcher / ai_cycle 互斥运行（VO/OSD/相机通道独占），跑 dara 脚本前先停掉它们

## 安装

```bash
# 开发机：构建 wheel（在本目录执行，产物在 dist/）
python3 -m pip install build
python3 -m build --wheel

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

两个模块级函数覆盖常见场景，`rear=True` 切后摄：

### `camera.read(rear=False, timeout_ms=1000)`

从 ai 通道（640×640，RGB888_PLANAR）取一帧 zero-copy `Frame`，**配合 nn 推理使用**：

```python
with camera.read() as frame:     # 帧出 with 块即失效
    r = model.run(frame)         # 推理必须在块内完成
```

### `camera.read_image(rear=False, timeout_ms=1000)`

从 rgb 通道（720×480）取一帧并转成紧凑 RGB `Image`，**配合 image 模块传统视觉使用**：

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

- `image.show(img)` / `img.show()`：把 RGBA Image 叠上屏幕（双缓冲 OSD
  直绘，盖在 UI 之上）。首次调用把 img 收编进 OSD 显存（此后绘制零拷贝），
  每次调用提交一帧；双缓冲两块画布内容独立，**每帧先 `clear()` 整幅重画**。
  只提交叠加层、不推进 UI；画布是 B,G,R,A 字节序，要屏幕红色传
  `color=(0, 0, 255, 255)`：

```python
from dara import image
import time

img = image.new(size=(720, 480), mode="RGBA")
while True:
    img.clear()
    img.draw_rectangle(100, 100, 300, 260, color=(0, 0, 255, 255), thickness=3)
    image.show(img)              # 或 img.show()
    time.sleep(0.03)
```

- `img.to_lv(parent)` 把 Image 零拷贝显示为 LVGL 控件；等价写法
  `w = lv.image(scr); w.set_src(img)`。Image 像素被控件借用，需保活。

## lv —— 嵌入 LVGL

```python
from dara import lv
```

`lv.*` 即 LVGL v9 API（转发到嵌入解释器，首次访问自动建显示通路）。控件、
布局、样式等用法参考 LVGL 中文文档：

<https://lvgl.100ask.net/>

dara 特有接口：

- `lv.show(fps=None)`：可选的帧率限速 / 显示 Image。UI 渲染（tick + 触摸）
  由 dara 内部线程自转，不调 show 界面也在跑；纯 UI 循环给 `fps` 防空转
- `lv.bind(obj, lv.EVENT.CLICKED, fn)`：LVGL 事件绑定到 CPython 无参回调
  （回调在 dara 的 UI 线程执行）
- 控件 `set_src(Image)`：直接显示 image 模块的 Image（见上节）

lv 可在任意线程调用（内部转交 UI 线程执行）。

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
**结果坐标已从推理帧映射到 720×480 屏幕坐标系**，可直接用于 LVGL / OSD 画框：

```python
with camera.read() as frame:
    r = model.run(frame)
for box in r.boxes:
    print(r.label_of(box.class_id), box.x1, box.y1, box.x2, box.y2)
```

### `Model.reset()`

显式卸载模型、释放 NPU 内存（可选；进程退出时系统兜底回收）。
