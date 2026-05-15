# Algorithms

This document describes the current algorithm-facing runtime layout,
postprocess behavior, and the intended extension points.

## Runtime Layers

Current algorithm implementation lives under:

- `src/algorithm/algorithm_engine.cpp`
- `src/algorithm/nn_base.cpp`
- `src/algorithm/nn_yolov5.cpp`
- `src/algorithm/nn_yolov8.cpp`
- `src/algorithm/nn_classifier.cpp`
- `src/algorithm/nn_feature.cpp`

Public wrappers are intentionally thinner than the runtime layer:

- `Detector`
- `Classifier`
- `FeatureExtractor`
- `Pipeline`

The wrapper layer decides which runtime to instantiate.
The runtime layer owns preprocessing, tensor decode, and postprocess.

## Current Algorithm Families

- `NnYolov5`
  Used for YOLOv5-style object detection models.
- `NnYolov8`
  Used for YOLOv8-style object detection models.
- `NnClassifier`
  Used for classification models that return ranked class scores.
- `NnFeature`
  Used for embedding / feature extraction models.

## Model Selection

The project does not depend on vendor `model_factory.json` at runtime.
Instead it resolves runtime selection from:

1. explicit `model_type` in `ModelSessionConfig`
2. `model_type` / `runtime` / `task_name` in `ModelDescriptor`
3. task defaults in `AlgorithmEngine`

Recommended user-facing construction:

```cpp
tdl_app::Detector det = tdl_app::Detector::yolov8();
tdl_app::Classifier cls = tdl_app::Classifier::generic();
tdl_app::FeatureExtractor feat = tdl_app::FeatureExtractor::generic();

auto cfg = tdl_app::ModelSessionConfig::fromSpec(
    "./configs/model_specs/yolov8n_det_coco80.ini",
    "./firmware/libbm1688_kernel_module.so",
    "./models");
```

## Postprocess Behavior

### YOLOv5

`NnYolov5` owns:

- model open
- image / native frame preprocess
- tensor decode
- confidence threshold filtering
- IoU-based NMS
- label mapping from descriptor

Expected output fields:

- `AlgorithmResult::boxes`
- `AlgorithmResult::labels`

### YOLOv8

`NnYolov8` owns:

- model open
- image / native frame preprocess
- branch/output interpretation for supported YOLOv8 layouts
- confidence threshold filtering
- IoU-based NMS
- label mapping from descriptor

Expected output fields:

- `AlgorithmResult::boxes`
- `AlgorithmResult::labels`

### Classifier

`NnClassifier` owns:

- input preprocess
- score vector decode
- top-k selection
- class label mapping

Expected output fields:

- `AlgorithmResult::classes`
- `AlgorithmResult::labels`

### Feature

`NnFeature` owns:

- input preprocess
- embedding tensor decode
- flat feature vector export

Expected output fields:

- `AlgorithmResult::feature`

## Threshold / IoU / Top-K

`InferOptions` currently means:

- `threshold`
  Detection confidence threshold, and a generic confidence floor for other runtimes.
- `iou_threshold`
  Used by detector runtimes during NMS.
- `top_k`
  Used by classifier-style postprocess.

The compatibility overload `runOnce(float threshold, ...)` is still supported,
but the preferred path is:

```cpp
tdl_app::InferOptions opt;
opt.threshold = 0.25f;
opt.iou_threshold = 0.45f;
opt.top_k = 5;
pipeline.runOnce(opt, &result, &error);
```

## Extension Rules

When adding a new algorithm family:

1. put runtime implementation under `src/algorithm/`
2. keep vendor/tensor/postprocess details inside the runtime class
3. expose a small public wrapper only if the family is meant to be stable
4. keep `AlgorithmResult` stable when possible
5. add a focused demo instead of extending a catch-all binary

## Recommended Next Families

Current codebase is structurally ready for:

- face detector wrapper
- pose / keypoint wrapper
- OCR detector/recognizer wrapper
- face feature + compare helper

Those should be added as distinct runtime classes instead of growing YOLO-specific code.
