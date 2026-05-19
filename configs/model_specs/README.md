# 模型描述文件说明

这个目录保存 `tdl_app_sdk` 使用的应用侧模型描述文件。

工程运行时不再依赖厂商侧 `model_factory.json`，而是完全依赖这里的 `.ini`。

## 基本格式

```ini
[basic]
type = bmodel
model = ../../models/cv184x/model.bmodel

[extra]
runtime = yolov5
task = detect
model_type = YOLOV5
input_type = rgb
preprocess = letterbox
mean = 0,0,0
scale = 0.0039215686,0.0039215686,0.0039215686
anchors = 10,13, 16,30
labels = class0,class1
```

## 当前常用字段

- `basic.model`：`bmodel` 路径
- `extra.runtime`：运行时类型，例如 `yolov5`、`yolov8`、`classifier`
- `extra.task`：任务提示，例如 `detect`、`classify`
- `extra.model_type`：暴露给上层的逻辑模型名
- `extra.input_type`：输入颜色格式提示，例如 `rgb`、`bgr`
- `extra.preprocess`：预处理提示，例如 `letterbox`、`resize`
- `extra.mean` / `extra.scale`：归一化参数
- `extra.anchors`：YOLOv5 anchor
- `extra.labels`：类别标签
- `extra.normalize`：特征后处理提示，例如 `l2`
- `extra.apply_softmax`：分类输出是否补 softmax

## 路径规则

`basic.model` 相对 `.ini` 文件自身所在目录解析。

在当前运行目录布局里，通常写成：

```ini
model = ../../models/cv184x/xxx.bmodel
```

## 一个检测模型示例

```ini
[basic]
type = bmodel
model = ../../models/cv184x/yolov5s_det_coco80_640_640_INT8_cv184x.bmodel

[extra]
runtime = yolov5
task = detect
model_type = YOLOV5_DET_COCO80
labels = person,bicycle,car
anchors = 10,13, 16,30, 33,23, 30,61, 62,45, 59,119, 116,90, 156,198, 373,326
```

## 与 firmware 的关系

`model_spec` 只描述模型本身，不包含板端 TPU kernel 动态库。

板端运行时如果需要 firmware，需要在 `ModelSessionConfig` 里额外传：

```cpp
tdl_app::ModelSessionConfig::fromSpec(
    "./configs/model_specs/yolov5s_det_coco80.ini",
    "./firmware/libbm1688_kernel_module.so",
    "./models");
```
