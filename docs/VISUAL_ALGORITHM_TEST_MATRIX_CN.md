# CV184X Algorithm Test Matrix

Exact board commands and expected results are maintained in
`CV184X_ALGORITHM_TEST_GUIDE_CN.md`.

This matrix is the release gate for the Dual-OS runtime. Native camera paths
must use MMF frame input and the self-developed VPSS preprocessor. Offline
image paths remain only for result comparison and annotation output.

## Ready To Test

| Algorithm | Test program | Model spec | Required input | Native path |
| --- | --- | --- | --- | --- |
| Classifier | `tdl_classify_demo` | `plant_classifier.mud` | `assets/plant.jpg`, or camera | VPSS |
| Feature | `tdl_camera_feature_demo`, `tdl_feature_demo` | `feature_cviface.mud` | face/person image, or camera | VPSS and VPSS ROI |
| SCRFD face detection | `tdl_camera_scrfd_benchmark_demo`, `tdl_face_detect_demo` | `scrfd_real.mud` | `assets/face.jpg`, or camera | VPSS |
| YOLOv5/YOLOv8 detection | `tdl_yolov5_demo`, `tdl_yolov8_demo`, camera benchmarks | `yolov5s_det_coco80.mud`, `yolov8n_det_coco80.mud` | `assets/dog.jpg`, camera | VPSS |
| YOLOv8 Pose | `tdl_keypoint_demo` | `pose_yolov8.mud` | `assets/keypoint.jpg`, or camera | VPSS letterbox |
| Human pose classification | `tdl_pose_classifier_demo` | `pose_yolov8.mud` | person image, or camera | VPSS letterbox + 17-point rules |
| SimCC Pose | `tdl_keypoint_demo` | `keypoint_simcc_person17.mud` | full frame for transport test; person ROI for accuracy test | VPSS |
| LSTR lane | `tdl_lane_demo` | `lane_lstr.mud` | `assets/road.jpg`, or road camera | VPSS |
| Face attribute | `tdl_face_attr_pipeline_demo`, `tdl_face_attribute_demo` | `face_attribute_gender_age_glass_emotion.mud` | `assets/face.jpg`, or camera | VPSS ROI |
| Semantic segmentation | `tdl_semantic_seg_demo` | `topformer_seg_person_face_vehicle.mud` | road/person image, or camera | VPSS |
| Instance segmentation | `tdl_instance_seg_demo` | `yolov8n_seg_coco80.mud` | COCO-like image, or camera | VPSS letterbox |
| Plate recognizer | `tdl_plate_recognize_demo` | `plate_recognizer_24x96.mud` | cropped plate image, or camera | VPSS |
| PP-OCR offline | `tdl_pp_ocr_demo` | `pp_ocr.mud` | document/plate image | CPU geometry only; camera path waits for GDC |
| ASR | `tdl_asr_demo`, `tdl_dualos_asr_demo` | `speech_zipformer_asr.mud` | 16 kHz, S16LE, mono PCM | audio, not VPSS |
| VAD | `tdl_vad_demo` | VAD FSMN spec | 16 kHz, S16LE, mono PCM | audio, not VPSS |
| Single object tracker | `tdl_single_object_tracker_demo` | `feartrack.mud` | camera/sequence plus initial box | persistent VPSS ROI + BMRT outputs |

## Board Commands

```sh
./run_single_object_tracker_demo.sh --camera \
  --model-spec ./configs/model_specs/feartrack.mud \
  --init-box 140,200,300,460 --group 0 --channel 1 \
  --warmup 5 --frames 300 \
  --dump-frame /tmp/feartrack_input.jpg \
  --dump-overlay /tmp/feartrack_overlay.jpg

./run_pose_classifier_demo.sh --camera \
  --model-spec ./configs/model_specs/pose_yolov8.mud \
  --group 0 --channel 1 --warmup 5 --frames 300 \
  --dump-frame /tmp/pose_input.jpg \
  --dump-overlay /tmp/pose_overlay.jpg

```

Each command prints FPS and stage averages. The MobileCLIP2 self-learning detector
is intentionally excluded from CV184X: it exceeds the available BPU carveout.

## Release Checks

1. Offline image result and camera frame result must be visually compared for
   detection, pose, lane, segmentation and OCR.
2. Run every camera test with at least 300 frames, then run the process ten
   times. No VPSS group exhaustion, retained frame or growing RSS is allowed.
3. For cascade models, verify that secondary inference uses a detector ROI and
   does not map the source `VIDEO_FRAME_INFO_S` to CPU memory.
4. Record model-specific thresholds and expected labels with the supplied
   validation data.

## Required Resources

The following assets are not available in the current runtime package and are
required to complete accuracy and regression validation.

| Resource | Minimum contents | Used by |
| --- | --- | --- |
| Person pose set | 20 images plus COCO-17 keypoints and person boxes | SimCC and YOLOv8 Pose |
| Face dense set | 20 face images plus 478 landmark annotations | dense face landmark |
| Hand set | 20 hand images plus 21 landmarks and optional hand boxes | hand keypoint |
| Plate set | 20 images, plate boxes, plate text and 4-corner annotations | plate recognizer, plate keypoint, PP-OCR |
| Tracker sequence | one template image, 100 ordered frames, initial box and per-frame boxes | FearTrack |
| Road lane set | 20 road images plus expected left/right lanes | LSTR |
| Segmentation set | images plus class/instance masks | semantic and instance segmentation |
| OCR set | 20 document/plate images plus UTF-8 transcripts | PP-OCR |
| ASR set | 16 kHz S16LE mono PCM plus UTF-8 reference text | Zipformer ASR |
| VAD set | 16 kHz S16LE mono PCM plus speech start/end labels | VAD |

## Missing Model Files

The current runtime package does not contain the following required resources:

* `recognition_speech_zipformer_encoder-s_71_80_BF16.bmodel`.
* `recognition_speech_zipformer_decoder-s_1_2_BF16.bmodel`.
* `recognition_speech_zipformer_joiner-s_1_512_1_512_BF16.bmodel`.
* Zipformer `tokens.txt`.
* A VAD FSMN BModel and its final non-template model spec.
* Road YOLOv5/YOLOv8 and YOLOv8 OBB BModels referenced by their specs.
* A packaged ByteTrack executable and its full regression test.

Dense face landmark, FearTrack, hand keypoint and PP-OCR model resources are
included in the current complete package.
