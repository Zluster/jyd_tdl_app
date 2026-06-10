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
- `src/algorithm/nn_scrfd.cpp`
- `src/algorithm/nn_face_attribute.cpp`
- `src/algorithm/nn_plate_recognizer.cpp`

Public wrappers are intentionally thinner than the runtime layer:

- `Detector`
- `Classifier`
- `FaceDetector`
- `FaceAttributeClassifier`
- `PlateRecognizer`
- `FeatureExtractor`
- `KeypointDetector`
- `SemanticSegmenter`
- `InstanceSegmenter`
- `LaneDetector`
- `VoiceActivityDetector`
- `Pipeline`
- `MultiStagePipeline`

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
- `NnScrfd`
  Used for SCRFD-style face detection with 5-point landmarks.
- `NnFaceAttribute`
  Used for face attribute classification on a full image or explicit face crop.
- `NnPlateRecognizer`
  Used for OCR-style license plate recognition.
- `KeypointDetector`
  Used for `tdl_sdk` keypoint models such as hand/person pose.
- `SemanticSegmenter`
  Used for `tdl_sdk` semantic segmentation models.
- `InstanceSegmenter`
  Used for `tdl_sdk` instance segmentation models.
- `LaneDetector`
  Used for `tdl_sdk` lane detection models.
- `VoiceActivityDetector`
  Used for `tdl_sdk` VAD FSMN models on raw PCM16LE mono audio.

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
tdl_app::FaceDetector face = tdl_app::FaceDetector::scrfd();
tdl_app::FaceAttributeClassifier attr =
    tdl_app::FaceAttributeClassifier::generic();
tdl_app::PlateRecognizer lpr = tdl_app::PlateRecognizer::lpr();
tdl_app::FeatureExtractor feat = tdl_app::FeatureExtractor::generic();

auto cfg = tdl_app::ModelSessionConfig::fromSpec(
    "./configs/model_specs/yolov8n_det_coco80.mud",
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

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./run_classify_demo.sh \
  --image /mnt/sd/hand.jpg \
  --model-spec ./configs/model_specs/cls_hand_gesture.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --top-k 5 \
  --output /mnt/sd/out_cls.jpg
```

Current packaged classifier demo model-spec:

- `configs/model_specs/cls_hand_gesture.mud`

### SCRFD Face Detector

`NnScrfd` owns:

- face detector image preprocess
- SCRFD branch/output discovery
- 5-point landmark decode
- face-box NMS

Expected output fields:

- `AlgorithmResult::boxes`
- `AlgorithmResult::boxes[i].landmarks`

Template model-spec:

- `configs/model_specs/template_scrfd.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./bin/tdl_face_detect_demo \
  --image /mnt/sd/face.jpg \
  --model-spec ./configs/model_specs/template_scrfd.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.35 \
  --output /mnt/sd/out_face.jpg
```

### Face Attribute

`NnFaceAttribute` owns:

- face crop resize preprocess
- multi-head output mapping for `gender/age/glasses/mask/emotion`
- attribute export into a stable `AlgorithmResult`

Expected output fields:

- `AlgorithmResult::attributes`

Template model-spec:

- `configs/model_specs/template_face_attribute.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./bin/tdl_face_attribute_demo \
  --image /mnt/sd/face_crop.jpg \
  --model-spec ./configs/model_specs/template_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

If you want to classify a face region inside a larger image:

```sh
./bin/tdl_face_attribute_demo \
  --image /mnt/sd/people.jpg \
  --model-spec ./configs/model_specs/template_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --roi 120,80,160,160
```

Face detect + attribute chained example:

```sh
./bin/tdl_face_attr_pipeline_demo \
  --image /mnt/sd/people.jpg \
  --detector-model-spec ./configs/model_specs/template_scrfd.mud \
  --attribute-model-spec ./configs/model_specs/template_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.35 \
  --output /mnt/sd/out_face_pipeline.jpg
```

### Plate Recognizer

`NnPlateRecognizer` owns:

- OCR recognizer image/crop preprocess
- CTC-like greedy decode
- plate text export

Expected output fields:

- `AlgorithmResult::text`

Template model-spec:

- `configs/model_specs/template_plate_recognizer.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./bin/tdl_plate_recognize_demo \
  --image /mnt/sd/plate.jpg \
  --model-spec ./configs/model_specs/template_plate_recognizer.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

### Feature

`NnFeature` owns:

- input preprocess
- embedding tensor decode
- flat feature vector export

Expected output fields:

- `AlgorithmResult::feature`

### Keypoint

`KeypointDetector` owns:

- model open through `TDL_OpenModel`
- image/native-frame wrapping through `TDL_ReadImage` / `TDL_WrapFrame`
- point conversion from normalized `TDLKeypoint`

Expected output fields:

- `KeypointResult::points`

Template model-spec:

- `configs/model_specs/template_keypoint_hand.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./run_keypoint_demo.sh \
  --image /mnt/sd/hand.jpg \
  --model-spec ./configs/model_specs/template_keypoint_hand.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

### Semantic Segmentation

`SemanticSegmenter` owns:

- model open through `TDL_OpenModel`
- image/native-frame wrapping through `TDL_ReadImage` / `TDL_WrapFrame`
- output mask conversion from `TDLSegmentation`

Expected output fields:

- `SemanticSegmentationResult::class_id`
- `SemanticSegmentationResult::class_conf`

Template model-spec:

- `configs/model_specs/template_semantic_segmentation.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./run_semantic_seg_demo.sh \
  --image /mnt/sd/scene.jpg \
  --model-spec ./configs/model_specs/template_semantic_segmentation.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

### Instance Segmentation

`InstanceSegmenter` owns:

- model open through `TDL_OpenModel`
- image/native-frame wrapping through `TDL_ReadImage` / `TDL_WrapFrame`
- instance, contour, and mask conversion from `TDLInstanceSeg`

Expected output fields:

- `InstanceSegmentationResult::instances`

Template model-spec:

- `configs/model_specs/template_instance_segmentation.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./run_instance_seg_demo.sh \
  --image /mnt/sd/dog.jpg \
  --model-spec ./configs/model_specs/template_instance_segmentation.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

### Lane Detection

`LaneDetector` owns:

- model open through `TDL_OpenModel`
- image/native-frame wrapping through `TDL_ReadImage` / `TDL_WrapFrame`
- lane segment conversion from `TDLLane`

Expected output fields:

- `LaneDetectionResult::lanes`
- `LaneDetectionResult::lane_state`

Template model-spec:

- `configs/model_specs/template_lane_detection.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./run_lane_demo.sh \
  --image /mnt/sd/road.jpg \
  --model-spec ./configs/model_specs/template_lane_detection.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

### Voice Activity Detection

`VoiceActivityDetector` owns:

- model open through `TDL_OpenModel(..., TDL_MODEL_VAD_FSMN, ...)`
- PCM16LE mono frame wrapping through `TDL_ReadAudioFrame`
- VAD segment conversion from `TDLVAD`

Expected output fields:

- `VoiceActivityResult::segments`
- `VoiceActivityResult::has_speech`
- `VoiceActivityResult::start_event`
- `VoiceActivityResult::end_event`

Template model-spec:

- `configs/model_specs/template_vad_fsmn.mud`

Quick CLI smoke test on the board:

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh

./run_vad_demo.sh \
  --pcm /mnt/sd/sample_16k_mono.pcm \
  --model-spec ./configs/model_specs/template_vad_fsmn.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --chunk-bytes 3200
```

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

## Multi-Stage Chaining

`MultiStagePipeline` is the current application-facing chaining layer for
image-based workflows where one stage consumes boxes from a previous stage.

Current supported stage shapes are:

- frame stage: detector / classifier / feature / SCRFD face detector
- crop stage: face attribute from face boxes
- crop stage: plate recognizer from detector boxes

Current limitation:

- crop stages currently require `Frame::image_path`
- native camera-frame crop chaining is not implemented yet

## Extension Rules

When adding a new algorithm family:

1. put runtime implementation under `src/algorithm/`
2. keep vendor/tensor/postprocess details inside the runtime class
3. expose a small public wrapper only if the family is meant to be stable
4. keep `AlgorithmResult` stable when possible
5. add a focused demo instead of extending a catch-all binary

## Recommended Next Families

Current codebase is structurally ready for:

- OCR detector wrapper
- face feature + compare helper

Those should be added as distinct runtime classes instead of growing YOLO-specific code.
