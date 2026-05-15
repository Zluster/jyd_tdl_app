# Model Specs

This directory contains application-level model description files.
They are the only model metadata source used by `tdl_app_sdk`.

Format:

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
mean = 0, 0, 0
scale = 0.00392156862745098, 0.00392156862745098, 0.00392156862745098
anchors = 10,13, 16,30
labels = class0,class1
```

Fields used today:

- `basic.model`: bmodel path
- `extra.runtime`: runtime family such as `yolov5`, `yolov8`, `classifier`, `feature`
- `extra.task`: optional task hint such as `detect`, `classify`, `feature`
- `extra.model_type`: logical model name exposed to upper layers
- `extra.input_type`: `rgb` or `bgr`
- `extra.preprocess`: runtime hint such as `letterbox` or `resize`
- `extra.mean` / `extra.scale`: per-channel preprocessing parameters
- `extra.anchors`: YOLOv5 anchors
- `extra.labels`: class labels
- `extra.normalize`: feature postprocess hint such as `l2`
- `extra.apply_softmax`: classifier output hint

`tdl_app_sdk` no longer depends on any vendor-side `model_factory.json`.

`basic.model` is resolved relative to the `.ini` file location. Under the
runtime package layout, specs live in `configs/model_specs/`, so model files
should normally be written as `../../models/cv184x/xxx.bmodel`.

Detection example:

```ini
[basic]
type = bmodel
model = ../../models/cv184x/yolov5s_det_coco80_640_640_INT8_cv184x.bmodel

[extra]
runtime = yolov5
task = detect
model_type = YOLOV5_DET_COCO80
```
