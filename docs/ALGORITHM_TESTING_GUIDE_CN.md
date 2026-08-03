# 算法测试与 `model_tool` 分析指南

## 0. 产品截图算法测试矩阵（2026-07-15）

本节是当前板端验收的唯一范围：通用分类、通用检测、人脸检测/识别/属性/478 点、人体关键点、
人体姿态分类、手关键点、手势分类、OCR、多目标跟踪/计数、自学习分类器、自学习检测/跟踪。
功能测试必须生成效果图；性能测试统一使用 `warmup=30`、`frames=300` 并打印 `avg_total_ms` 和 `fps`。

| 功能 | 功能图 | 性能测试 | 当前状态 |
| --- | --- | --- | --- |
| 通用分类 | 分类标签/分数 overlay | 相机 300 帧 | 可运行 |
| 通用检测 | 框/类别/分数 overlay | 相机 300 帧 | 可运行 |
| 人脸检测 | 框+5点 overlay | 相机 300 帧 | 可运行 |
| 人脸识别 | 匹配框和 similarity 图 | 离线 `repeat=300` | 仅离线，待相机化 |
| 人脸属性 | 属性 overlay | 相机 300 帧 | 可运行 |
| 478 人脸点 | 478 点 overlay | 离线和相机各 300 帧 | 可运行 |
| 人体关键点/姿态分类 | 17 点或姿态标签 overlay | 相机 300 帧 | 可运行 |
| 手部关键点 | 21 点 overlay | 相机 300 帧 | 可运行 |
| 手势分类 | 手势标签 overlay | 手部 ROI 相机 300 帧 | 待开发统一 pipeline |
| OCR | 文本框和文本 overlay | 检测/识别分阶段 300 帧 | 可运行；检测 VPSS，四点矫正 CPU |
| 多目标跟踪计数 | ID/轨迹/计数 overlay | detector/tracker/count 300 帧 | 可运行 |
| 自学习图像分类 | top-k 结果图 | 离线图片 feature bank（非 FPS） | 仅离线功能可验收 |
| 自学习检测 | 标签/相似度 overlay | 不适用 | 当前不可验收（特征模型超出 BPU 内存） |
| 自学习单目标跟踪 | 跟踪框 overlay | VPSS/BMRT/后处理 300 帧 | 可运行 |

### 0.1 可直接执行的功能和性能命令

先准备统一目录：

```sh
cd /root/jyd_tdl_app_sdk_cv184x_self
mkdir -p /tmp/jyd_results
```

#### 0.1.1 通用图像分类：植物五分类

- 测试内容：相机画面整帧分类，输出 top-5 类别和分数。
- 使用模型：`plant_classifier.mud`。
- 功能结果：查看 `classify_overlay.jpg` 左上角类别和概率。
- 性能结果：查看 `avg_read_ms`、`avg_infer_ms`、`avg_total_ms`、`fps`。

```sh
./bin/tdl_camera_classify_demo \
  --model-spec ./configs/model_specs/plant_classifier.mud \
  --group 0 --channel 1 --warmup 30 --frames 300 --top-k 5 \
  --dump-frame /tmp/jyd_results/classify_input.jpg \
  --dump-overlay /tmp/jyd_results/classify_overlay.jpg
```

#### 0.1.2 通用目标检测：YOLOv8 COCO80

- 测试内容：检测 person、book、cell phone 等 COCO 目标。
- 使用模型：`yolov8n_det_coco80.mud`；测试 YOLOv5 时替换为 `yolov5s_det_coco80.mud`。
- 功能结果：查看 `det_overlay.jpg` 中目标框、类别和分数。
- 性能结果：查看 `avg_read_ms`、`avg_infer_ms`、`avg_total_ms`、`fps`、`avg_boxes`。

```sh
./bin/tdl_camera_detect_demo \
  --model-spec ./configs/model_specs/yolov8n_det_coco80.mud \
  --group 0 --channel 1 --warmup 30 --frames 300 \
  --dump-frame /tmp/jyd_results/det_input.jpg \
  --dump-overlay /tmp/jyd_results/det_overlay.jpg
```

#### 0.1.3 人脸检测：SCRFD 人脸框和 5 点

- 测试内容：检测人脸框、双眼、鼻尖和双嘴角。
- 使用模型：`scrfd_real.mud`。
- 功能结果：查看 `scrfd_overlay.jpg`，5 点必须落在人脸对应位置。
- 性能结果：查看 SCRFD 预处理、BMRT、后处理、总耗时和 FPS。

```sh
./bin/tdl_camera_scrfd_benchmark_demo \
  --model-spec ./configs/model_specs/scrfd_real.mud \
  --group 0 --channel 1 --warmup 30 --frames 300 --dump-boxes \
  --dump-frame /tmp/jyd_results/scrfd_input.jpg \
  --dump-overlay /tmp/jyd_results/scrfd_overlay.jpg
```

#### 0.1.4 人脸情绪和属性：SCRFD + 属性模型

- 测试内容：先检测人脸，再对每张脸识别性别、年龄、眼镜和情绪。
- 使用模型：`scrfd_real.mud` + `face_attribute_gender_age_glass_emotion.mud`。
- 功能结果：查看 `face_attr_overlay.jpg` 中每张脸的属性文字。
- 性能结果：查看 `avg_detect_ms`、`avg_attribute_ms`、`avg_total_ms`、`fps`。

```sh
./bin/tdl_face_attr_pipeline_demo --camera \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --attribute-model-spec ./configs/model_specs/face_attribute_gender_age_glass_emotion.mud \
  --threshold 0.25 --group 0 --channel 1 --frames 300 \
  --dump-frame /tmp/jyd_results/face_attr_input.jpg \
  --dump-overlay /tmp/jyd_results/face_attr_overlay.jpg
```

#### 人脸识别：离线功能和 pair 性能

- 测试内容：参考图与查询图分别做人脸检测、对齐、特征提取和余弦相似度比较。
- 使用模型：`scrfd_real.mud` + `feature_cviface.mud`。
- 功能结果：查看 `face_recognition.jpg` 中人脸框、`match/different` 和 similarity。
- 性能结果：查看 `avg_reference_ms`、`avg_query_ms`、`avg_pair_ms`、`pairs_per_second`。
- 限制：当前是离线文件性能，不代表相机 VPSS/GDC 硬件链路。

```sh
./bin/tdl_face_recognition_demo \
  --reference-image ./assets/face.jpg \
  --query-image ./assets/face.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --feature-model-spec ./configs/model_specs/feature_cviface.mud \
  --match-threshold 0.50 --repeat 300 \
  --output /tmp/jyd_results/face_recognition.jpg
```

#### 0.1.5 人脸稠密关键点：478 点相机硬件路径

- 测试内容：SCRFD 检测后，用 VPSS 正方形 ROI 推理 478 点。
- 使用模型：`scrfd_real.mud` + `face_dense_real.mud`。
- 功能结果：查看 `face478_overlay.jpg`，点应覆盖轮廓、眉眼、鼻子和嘴，不能向右下漂移。
- 性能结果：查看 `avg_detect_ms`、`avg_vpss_roi_ms`、`avg_bmrt_ms`、
  `avg_output_copy_decode_ms`、`avg_postprocess_ms`、`avg_total_ms`、`fps`。

```sh
./run_face_dense_keypoint_demo.sh --camera \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --keypoint-model-spec ./configs/model_specs/face_dense_real.mud \
  --threshold 0.25 --roi-expand-ratio 0.2 \
  --group 0 --channel 1 --warmup 30 --frames 300 \
  --dump-frame /tmp/jyd_results/face478_input.jpg \
  --dump-overlay /tmp/jyd_results/face478_overlay.jpg
```

#### 0.1.6 人体关键点：YOLOv8 Pose 17 点

- 测试内容：检测人体框并输出 COCO 17 点骨架。
- 使用模型：`pose_yolov8.mud`。
- 功能结果：查看 `body17_overlay.jpg`，肩、肘、腕、髋、膝和踝应贴合人体。
- 性能结果：查看 `avg_read_ms`、`avg_infer_ms`、`avg_total_ms`、`fps`。

```sh
./bin/tdl_keypoint_demo --camera \
  --model-spec ./configs/model_specs/pose_yolov8.mud \
  --group 0 --channel 1 --frames 300 \
  --dump-frame /tmp/jyd_results/body17_input.jpg \
  --dump-overlay /tmp/jyd_results/body17_overlay.jpg
```

#### 0.1.7 人体姿态分类：17 点规则分类

- 测试内容：根据 17 点输出站、坐、躺、举手等姿态并做时序平滑。
- 使用模型：`pose_yolov8.mud` + 自研规则后处理。
- 功能结果：查看 `pose_class_overlay.jpg` 中姿态标签。
- 性能结果：查看关键点、平滑、规则分类、总耗时和 FPS。

```sh
./bin/tdl_pose_classifier_demo --camera \
  --model-spec ./configs/model_specs/pose_yolov8.mud \
  --group 0 --channel 1 --warmup 30 --frames 300 \
  --dump-frame /tmp/jyd_results/pose_class_input.jpg \
  --dump-overlay /tmp/jyd_results/pose_class_overlay.jpg
```

#### 手部关键点：21 点

- 功能测试：使用已经裁好的手部图片，检查手腕和五根手指的 21 点顺序。
- 性能测试：相机整帧命令只测 VPSS/BMRT 吞吐；准确率验收必须接手部检测 ROI。
- 使用模型：`keypoint_hand_128.mud`。
- 性能结果：查看 `avg_read_ms`、`avg_infer_ms`、`avg_total_ms`、`fps`。

```sh
# 功能测试
./bin/tdl_keypoint_demo \
  --image /tmp/hand_crop.jpg \
  --model-spec ./configs/model_specs/keypoint_hand_128.mud \
  --output /tmp/jyd_results/hand21_function.jpg

# 性能测试；不用于整帧手关键点准确率结论
./bin/tdl_keypoint_demo --camera \
  --model-spec ./configs/model_specs/keypoint_hand_128.mud \
  --group 0 --channel 1 --frames 300 \
  --dump-frame /tmp/jyd_results/hand21_input.jpg \
  --dump-overlay /tmp/jyd_results/hand21_overlay.jpg
```

#### 0.1.8 自学习检测：当前不可验收

YOLO proposal + feature bank 匹配依赖的 MobileCLIP2 约需 92.5 MiB BPU device memory，超过
CV184X 的 75 MiB carveout。因此该实验性入口、模型规格和相机自学习分类入口均不随当前包发布，
也不能用于性能验收；这不是 VPSS、后处理或样本库故障。

实时自学习单目标场景请使用下一节 FearTrack。若后续提供通用的 no-top INT8 embedding bmodel，
应优先选择 device memory 小于 20 MiB、224x224 RGB UINT8/INT8 输入、输出 256 或 512 维特征的模型，
再重新启用该 pipeline。

#### 0.1.9 自学习单目标跟踪：FearTrack

- 测试内容：第一帧手工给框，后续帧持续跟踪同一目标。
- 使用模型：`feartrack.mud`；`--init-box` 必须按现场目标修改。
- 功能结果：查看 `feartrack_overlay.jpg` 中跟踪框和置信度。
- 性能结果：查看 `avg_vpss_roi_ms`、`avg_bmrt_ms`、`avg_output_copy_ms`、
  `avg_postprocess_ms`、`avg_total_ms`、`fps`。
- 初始框应紧贴目标；框在第一帧偏离目标时，应先调整 `--init-box`，不能据此判断模型跟踪效果。
- 300 帧验收时应有 `initialize_ms`、各分阶段平均耗时和 `fps`；目标缓慢移动时框应连续跟随，
  短时遮挡可降低 score，但不应立即跳到背景。

```sh
./bin/tdl_single_object_tracker_demo --camera \
  --model-spec ./configs/model_specs/feartrack.mud \
  --init-box 180,120,360,420 --group 0 --channel 1 \
  --warmup 30 --frames 300 \
  --dump-frame /tmp/jyd_results/feartrack_input.jpg \
  --dump-overlay /tmp/jyd_results/feartrack_overlay.jpg
```

#### 0.1.10 人脸 478 点离线功能和离线性能

- 功能命令：单次运行，检查固定图片上的点位正确性。
- 性能命令：同一图片运行 warmup 30 次、计时 300 次。
- 性能结果：查看 `offline_runs`、`avg_detect_ms`、`avg_crop_align_ms`、
  `avg_dense_infer_ms`、`avg_postprocess_ms`、`avg_total_ms`、`fps`。

```sh
# 功能测试
./run_face_dense_keypoint_demo.sh \
  --image /mnt/sd/test.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --keypoint-model-spec ./configs/model_specs/face_dense_real.mud \
  --threshold 0.25 --roi-expand-ratio 0.2 \
  --output /tmp/jyd_results/face478_function.jpg

# 性能测试
./run_face_dense_keypoint_demo.sh \
  --image /mnt/sd/test.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --keypoint-model-spec ./configs/model_specs/face_dense_real.mud \
  --threshold 0.25 --roi-expand-ratio 0.2 --warmup 30 --frames 300 \
  --output /tmp/jyd_results/face478_perf.jpg
```

#### 0.1.11 OCR：PP-OCR 文本检测和识别

- 测试内容：相机整帧先做 PP-OCR 文本检测，再对每个旋转文本框做四点矫正和 CTC 识别。
- 使用资源：`pp_ocr.mud`、`ch_PP_OCRv3_det_int8_sym.bmodel`、
  `ch_PP_OCRv4_rec_int8_sym.bmodel`、`configs/ppocr_keys_v1.txt`。
- 硬件路径：检测 resize/letterbox/归一化由常驻 VPSS 完成，检测和识别由常驻 BMRT 完成。
- 当前限制：CV184X 公开 GDC API 没有开放任意 homography task，因此旋转文本框四点矫正使用
  CPU `warpPerspective`，耗时单独打印为 `avg_rectify_cpu_ms`，不能将其当成 GDC 耗时。
- 功能结果：查看 `ocr_overlay.jpg` 中绿色四点文本框和 UTF-8 完整识别文本；中文由包内
  `fonts/DroidSansFallbackFull.ttf` 通过 FreeType 绘制，不再使用 OpenCV Hershey 乱码占位。
- 性能结果：查看 `hardware_det_preprocess=1`、`avg_det_preprocess_ms`、
  `avg_det_inference_ms`、`avg_det_postprocess_ms`、`avg_rectify_cpu_ms`、
  `avg_rec_preprocess_ms`、`avg_rec_inference_ms`、`avg_rec_decode_ms`、`fps`。

固定素材功能测试：

```sh
./scripts/run_pp_ocr_function_test.sh
```

相机硬件性能测试：

```sh
./scripts/run_pp_ocr_camera_benchmark.sh
```

预期现象：终端至少打印 `frames=300 hardware_det_preprocess=1` 和 FPS；镜头中放置清晰、
占画面足够面积的 `assets/ocr_test_card.jpg` 后，`avg_text_regions` 应大于 0，文本框方向和
位置应贴合文字行。功能图为 `/tmp/jyd_results/ocr_function_overlay.jpg`，相机图为
`/tmp/jyd_results/ocr_camera_overlay.jpg`。

#### 0.1.12 多目标跟踪和过线计数：YOLOv8 + ByteTrack

- 测试内容：YOLOv8 检测 person，ByteTrack 进行高分框第一次关联、低分框第二次关联，
  输出稳定 ID、历史轨迹和左右方向过线计数。
- 实现方式：参考 `tdl_sdk` MOT 的状态生命周期，自研 8 状态常速度 Kalman 预测、Hungarian
  全局分配、高/低分两阶段关联、仅高分框建轨和超时回收；不链接 `tdl_sdk` 或 Eigen 代码。
  高密度交叉场景仍应单独记录 ID switch，不能只看平均 FPS。
- 使用资源：`yolov8n_det_coco80.mud` 及其 bmodel；不需要额外 ByteTrack 模型。
- 功能结果：查看 `bytetrack_overlay.jpg` 中 `ID`、轨迹、黄色计数线及 `L->R/R->L`。
- 性能结果：查看 `avg_read_ms`、`avg_detect_ms`、`avg_tracker_ms`、`avg_total_ms`、
  `fps`、`avg_detections`、`avg_active_tracks`。
- 参数说明：默认 `--class-id 0` 只跟踪 COCO person；`--line-x 320` 是 640 宽画面中线，
  可按现场修改。测试时让两个人分别从计数线两侧穿过。

固定 100 帧素材功能测试：

```sh
./scripts/run_bytetrack_function_test.sh
```

相机硬件性能测试：

```sh
./scripts/run_bytetrack_camera_benchmark.sh
```

预期现象：同一行人在连续帧和短时遮挡后的 ID 应尽量保持不变；穿越黄色线后对应方向计数加一；
`avg_tracker_ms` 应明显小于 `avg_detect_ms`。固定素材效果图为
`/tmp/jyd_results/bytetrack_function_overlay.jpg`，现场走位参考
`assets/bytetrack_camera_test_guide.jpg`。

#### 0.1.13 自学习图像分类：离线 feature bank top-k 匹配

- 测试内容：样本图片提取 512 维 CLIP 特征，L2 归一化后保存为 feature bank；查询图片与各类别
  原型做 cosine similarity 并输出 top-k。类别由 `--add 标签=图片` 定义，不是固定分类标签。
- 使用资源：`feature_clip_image.mud` 及其 bmodel；示例样本使用包内 `assets/dog.jpg`、
  `assets/plant.jpg` 和 `assets/self_learning_dog_query.jpg`。
- 功能结果：终端打印 `feature_dim: 512`、`classes=2 samples=2` 和 top-k；dog 查询中 `dog` 应排在
  `plant` 前，生成 `/tmp/jyd_results/self_classifier_function_overlay.jpg`。
- 已验证 dog 查询参考分数为 `dog=0.60862`、`plant=0.29737`；分数会随构图变化，验收重点是 dog 排名第一。
- 采样要求：每类提供 3 至 10 张主体清晰、覆盖实际视角/尺度/光照的图片；同标签样本会求均值原型。

固定素材 top-k 功能测试：

```sh
./scripts/run_self_classifier_function_test.sh
```

手工建立并保存 feature bank：

```sh
./bin/tdl_self_learn_classify_demo \
  --model-spec ./configs/model_specs/feature_clip_image.mud \
  --bank /tmp/jyd_results/my_feature.bank \
  --add dog=./assets/dog.jpg \
  --add plant=./assets/plant.jpg \
  --image ./assets/self_learning_dog_query.jpg --top-k 2 \
  --output /tmp/jyd_results/my_feature_overlay.jpg
```

后续查询复用同一个 bank，不再传入 `--add`：

```sh
./bin/tdl_self_learn_classify_demo \
  --model-spec ./configs/model_specs/feature_clip_image.mud \
  --bank /tmp/jyd_results/my_feature.bank \
  --image ./assets/self_learning_dog_query.jpg --top-k 2 \
  --output /tmp/jyd_results/my_feature_reuse_overlay.jpg
```

`feature_clip_image.mud` 约需 61.7 MiB BPU memory，只支持上述离线图片 feature bank；
相机路径不随当前包发布，不能据此给出 FPS。

FearTrack 的单算法连续 300 帧运行后，程序应正常退出并输出 FPS 和 `avg_total_ms`，不应出现持续增长的
VPSS group 数、BPU 分配失败或 `Segmentation fault`。多算法并发前应先分别完成单算法 300 帧测试；
512 MiB 系统内存和 75 MiB BPU carveout 不适合同时常驻多个大型特征模型。

三项素材的详细清单、预期图和重建方式见 `docs/THREE_ALGORITHM_TEST_ASSETS_CN.md`；对应完整
源码快照位于包内 `source/jyd_tdl_app/`。

#### 尚无可执行性能命令的截图项目

以下项目只有功能入口或底层类，尚无同时满足“效果图 + 分阶段耗时 + FPS”的测试程序：

- 手势分类：需要手部检测 -> VPSS ROI -> 21 点/分类 -> 手势 overlay 的统一相机 pipeline。

这些项目实现前没有合格的性能命令，不能用 shell 外层计时或单帧离线命令代替。

本文面向当前 `tdl_app_sdk` 仓库，整理每个已接入算法的测试方法、对应 demo、典型命令、输出判读方式，以及如何借助 `model_tool` 分析模型输入输出并反推后处理逻辑。

## 1. 使用范围与前提

本文覆盖当前仓库中的算法类与对应 demo，重点包括：

- 检测：`Detector` / `FaceDetector`
- 分类：`Classifier`
- 特征：`FeatureExtractor`
- 人脸属性：`FaceAttributeClassifier`
- 车牌识别：`PlateRecognizer`
- 关键点：`KeypointDetector`
- 人脸稠密关键点：`tdl_face_dense_keypoint_demo`
- 实例分割：`InstanceSegmenter`
- 语义分割：`SemanticSegmenter`
- 单目标跟踪：`SingleObjectTracker`
- 车道线：`LaneDetector`
- 语音活动检测：`VoiceActivityDetector`

默认约定：

- 仓库根目录为 `/mnt/sd/tdl_app_sdk_cv184x` 或 `/home/jyd/zwz/sophpi/tdl_app_sdk`
- 已执行 `. ./env.sh`
- `firmware/libbm1688_kernel_module.so` 可用
- `configs/model_specs/*.mud` 与 `models/cv184x/*.bmodel` 路径匹配

如果你的本地分支还保留旧的 `.ini`，而 SDK 主机或板端已经切到 `.mud`，优先以板端当前实际存在的 `.mud` 为准。

## 2. 通用测试流程

推荐所有视觉算法都按下面顺序测试：

1. 先确认 `model_spec` 指向的 `.bmodel` 存在。
2. 用 `model_tool` 查看模型输入输出、shape、dtype、scale、zero point。
3. 再运行 demo，先看终端输出，再决定是否保存可视化结果图。
4. 如果结果异常，先判断是：
   - 模型本身输出不对
   - `model_spec` 配置不对
   - 后处理映射不对
   - 输入预处理不对

## 3. `model_tool` 怎么用

### 3.1 工具路径

当前 SDK 主机上的工具路径是：

```sh
/home/jyd/zwz/sophpi/tdl_app_sdk/configs/model_tool
```

它不是目录，而是一个 ELF 可执行文件。

### 3.2 常用命令

查看模型信息：

```sh
./configs/model_tool --info models/cv184x/xxx.bmodel
```

典型输出会包含：

- `input`: 输入名字、shape、dtype、scale、zero point
- `output`: 输出名字、shape、dtype、scale、zero point
- `device mem size`: TPU 显存开销
- `host mem size`: 主机侧内存开销

### 3.3 常见报错

如果在 SDK 主机上运行时报：

```sh
error while loading shared libraries: libomp.so.5
```

说明当前主机缺少 OpenMP 运行时。处理方式：

- 优先直接在板端运行 `model_tool`
- 或者在 SDK 主机补齐 `libomp.so.5`

### 3.4 怎么从 `model_tool` 输出判断后处理类型

#### 分类 / 特征

如果输出类似：

```text
output: xxx, [1, N]
```

通常表示：

- 分类：对 `N` 维分数做 `softmax` 或直接 `top-k`
- 特征：直接把 `N` 维向量导出，必要时做 `L2 normalize`

#### YOLOv5 / YOLOv8 检测

如果看到 3 个尺度输出，比如 `80x80`、`40x40`、`20x20`，通常是检测模型。

- YOLOv5：常见是 anchor-based 输出
- YOLOv8：常见是 DFL box 分支 + class 分支

#### SCRFD 人脸检测

如果输出是 3 组 stride 分支，并且每组有：

- score
- bbox
- kps

那么后处理通常是：

- 解 score
- 解 box
- 解 5 点 landmark
- 做 NMS

#### 关键点

如果输出直接是坐标或坐标+置信度，后处理通常是直接缩放回原图。

如果输出是检测头格式，则需要先解框，再解关键点。

#### 实例分割

如果输出里同时有：

- 检测框分支
- 类别分支
- mask coeff 分支
- proto 分支

那么通常是 YOLOv8/YOLO11 seg 类模型，后处理是：

1. 先解检测框
2. 再取对应 mask coeff
3. 与 proto 做线性组合
4. 上采样并裁切回原图

#### 语义分割

如果输出是：

- `ArgMax` 类整型图
- 或者 `C x H x W` 类别图

那么通常是逐像素分类。

#### 跟踪

如果模型有两个输入，例如：

- `template_image`
- `search_image`

并输出：

- `cls_Sigmoid`
- `bbox_Add`

那么通常是 Siamese 单目标跟踪结构，后处理是：

1. 初始化 template patch
2. 当前帧取 search patch
3. 从 score map 找峰值位置
4. 从 bbox map 回归目标框

#### VAD

如果输入不是图像而是 `pcm16le_mono`，并且输出与时间片段相关，就是语音活动检测，不属于视觉模型。

## 4. `model_spec` 关键字段怎么理解

示例：

```ini
[basic]
type = bmodel
model = ../../models/cv184x/pose_yolov8n_pose_cv184x_int8_sym.bmodel

[extra]
runtime = keypoint
task = keypoint
model_type = KEYPOINT_YOLOV8POSE
input_type = rgb
mean = 0,0,0
scale = 0.00392156862745098,0.00392156862745098,0.00392156862745098
```

字段说明：

- `basic.model`: 模型路径
- `extra.runtime`: 运行时类型，决定走哪个算法实现
- `extra.task`: 任务语义，供上层调度参考
- `extra.model_type`: 更具体的模型类别
- `extra.input_type`: `rgb` 或 `bgr`
- `extra.mean` / `extra.scale`: 预处理归一化
- `extra.labels`: 分类或检测标签
- `extra.apply_softmax`: 分类模型是否在后处理中补 softmax
- `extra.normalize`: 特征模型是否做 `l2`

## 5. 算法与 demo 总表

| 算法 | 主类 / 入口 | 主测试 demo | 结果形式 |
| --- | --- | --- | --- |
| 通用检测 | `Detector` | `tdl_detect_demo` | `boxes + labels` |
| YOLOv5 检测 | `Detector("YOLOV5")` | `tdl_yolov5_demo` / `tdl_detect_demo` | `boxes + labels` |
| YOLOv8 检测 | `Detector("YOLOV8")` | `tdl_yolov8_demo` / `tdl_detect_demo` | `boxes + labels` |
| 人脸检测 | `FaceDetector` | `tdl_face_detect_demo` | `face boxes + landmarks` |
| 分类 | `Classifier` | `tdl_classify_demo` | `classes + labels` |
| 特征提取 | `FeatureExtractor` | `tdl_feature_demo` | `feature vector` |
| 人脸属性 | `FaceAttributeClassifier` | `tdl_face_attribute_demo` | `attributes` |
| 车牌识别 | `PlateRecognizer` | `tdl_plate_recognize_demo` | `text` |
| 关键点 | `KeypointDetector` | `tdl_keypoint_demo` | `points` |
| 稠密人脸关键点 | 独立 demo | `tdl_face_dense_keypoint_demo` | `478 dense points` |
| 实例分割 | `InstanceSegmenter` | `tdl_instance_seg_demo` | `instances + mask` |
| 语义分割 | `SemanticSegmenter` | `tdl_semantic_seg_demo` | `class map` |
| 单目标跟踪 | `SingleObjectTracker` | `tdl_single_object_tracker_demo` | `tracked box` |
| 车道线 | `LaneDetector` | `tdl_lane_demo` | `lanes` |
| VAD | `VoiceActivityDetector` | `tdl_vad_demo` | `speech segments` |

说明：

- `tdl_detect_demo` 是当前最通用的检测入口，推荐优先使用。
- `tdl_yolov5_demo` 和 `tdl_yolov8_demo` 更像快捷包装。
- `tdl_face_dense_keypoint_demo.cpp` 当前存在源码，但不一定在默认 CMake 目标中启用，取决于你的分支。

## 6. 各算法测试命令与判读

下面命令默认在板端仓库根目录执行：

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
. ./env.sh
```

### 6.1 通用目标检测

推荐 spec：

- `configs/model_specs/yolov5s_det_coco80.mud`
- `configs/model_specs/yolov8n_det_coco80.mud`
- `configs/model_specs/yolov8n_det_roads.mud`

命令：

```sh
./bin/tdl_detect_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/yolov8n_det_coco80.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --output /mnt/sd/out_detect.jpg
```

输出重点：

- `boxes`: 检测框数量
- `id` / `score`: 类别与置信度
- `box=(x1,y1,x2,y2)`: 原图坐标

后处理重点：

- YOLOv5：anchor decode + NMS
- YOLOv8：DFL decode + NMS

### 6.2 人脸检测

推荐 spec：

- `configs/model_specs/scrfd_real.mud`

命令：

```sh
./bin/tdl_face_detect_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/scrfd_real.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --output /mnt/sd/out_face.jpg
```

输出重点：

- `boxes`
- `landmarks=5` 或 `landmarks=0`

判读：

- 如果 box 正常但 `landmarks=0`，通常是后处理没有匹配到 kps 输出，或者该路径当前没有解 5 点。

### 6.3 分类

推荐 spec：

- `configs/model_specs/cls_hand_gesture.mud`
- `configs/model_specs/plant_classifier.mud`

命令：

```sh
./bin/tdl_classify_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/plant_classifier.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --top-k 5 \
  --output /mnt/sd/out_cls.jpg
```

输出重点：

- `classes`
- `[i] id= score= label=`

后处理重点：

- 是否需要 `softmax`
- 标签顺序是否与训练导出一致
- 输入 `mean/scale` 是否和量化前预处理一致

### 6.4 特征提取

推荐 spec：

- `configs/model_specs/feature_cviface.mud`

命令：

```sh
./bin/tdl_feature_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/feature_cviface.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

输出重点：

- `feature elements`

判读：

- 主要看向量长度是否正确
- 如果后续做检索，要再验证是否已做 `l2 normalize`

### 6.5 人脸属性

推荐 spec：

- 使用当前实际的人脸属性 `.mud`
- 如果仓库里暂无现成 spec，可从 `template_face_attribute.mud/.ini` 派生

命令：

```sh
./bin/tdl_face_attribute_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --roi 40,30,120,140 \
  --output /mnt/sd/out_face_attr.jpg
```

输出重点：

- `attributes`

判读：

- 如果属性值整体固定不变，优先检查输出映射、softmax/sigmoid、ROI 裁切是否正确。

### 6.6 车牌识别

推荐 spec：

- 使用当前实际车牌识别 `.mud`
- 如果仓库里暂无现成 spec，可从 `template_plate_recognizer.mud/.ini` 派生

命令：

```sh
./bin/tdl_plate_recognize_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_plate_recognizer.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --roi 100,120,180,60 \
  --output /mnt/sd/out_plate.jpg
```

输出重点：

- `text`

后处理重点：

- 当前实现是 CTC greedy decode
- 重点检查字符表顺序、blank 去重逻辑、输入宽高是否匹配

### 6.7 一般关键点 / Pose

推荐 spec：

- `configs/model_specs/pose_yolov8.mud`

命令：

```sh
./bin/tdl_keypoint_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/pose_yolov8.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/out_keypoint.jpg
```

输出重点：

- `points`
- 每个点的 `x y score`

判读：

- 如果点整体偏移，优先检查：
  - 输入 resize / letterbox 映射
  - 输出坐标是否正确映射回原图
  - 点数是否由输出通道自动推断正确

### 6.8 稠密人脸关键点

推荐 spec：

- 检测器：`configs/model_specs/scrfd_real.mud` 或 `yolov8_face_real.mud`
- 稠密关键点：`configs/model_specs/face_dense_real.mud`

命令：

```sh
./run_face_dense_keypoint_demo.sh \
  --image ./assets/face.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --keypoint-model-spec ./configs/model_specs/face_dense_real.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --roi-expand-ratio 0.2 \
  --output /mnt/sd/out_face_dense.jpg
```

输出重点：

- `face_count`
- `dense_point_count`
- 调试信息中的 `roi=(affine_aligned,256x256)` 或其他 crop 模式

判读：

- 如果下巴、轮廓偏移，优先怀疑：
  - 5 点对齐矩阵
  - box-based fallback 裁切策略
  - 关键点输出坐标尺度映射

### 6.9 实例分割

推荐 spec：

- `configs/model_specs/segTest_yolov8n_seg.mud`
- `configs/model_specs/segTest_yolo11n_seg.mud`

命令：

```sh
./bin/tdl_instance_seg_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/segTest_yolov8n_seg.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/out_seg.jpg
```

调试命令：

```sh
TDL_APP_SEG_DEBUG=1 ./bin/tdl_instance_seg_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/segTest_yolov8n_seg.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/out_seg.jpg
```

输出重点：

- `instances`
- 每个实例的 `class_id score box outline_points`

后处理重点：

- 检测框 decode
- coeff/proto 对齐
- mask 上采样后裁切回原图

### 6.10 语义分割

推荐 spec：

- `configs/model_specs/your_topformer_seg.mud`
- 模板：`configs/model_specs/template_semantic_segmentation.mud`

命令：

```sh
./bin/tdl_semantic_seg_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_topformer_seg.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

输出重点：

- `output_width`
- `output_height`
- `pixels`

判读：

- 当前 demo 默认不生成彩色 mask 图
- 如果模型输出是 `ArgMax [1,H,W]`，当前最稳的检查方式是先确认像素数与输出尺寸匹配

### 6.11 单目标跟踪

推荐 spec：

- `configs/model_specs/your_feartrack.mud`

命令：

```sh
./bin/tdl_single_object_tracker_demo \
  --template-image /mnt/sd/template.jpg \
  --search-image /mnt/sd/search.jpg \
  --model-spec ./configs/model_specs/your_feartrack.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --init-box 100,80,180,220 \
  --output /mnt/sd/out_track.jpg
```

调试命令：

```sh
TDL_APP_TRACK_DEBUG=1 ./bin/tdl_single_object_tracker_demo \
  --template-image /mnt/sd/template.jpg \
  --search-image /mnt/sd/search.jpg \
  --model-spec ./configs/model_specs/your_feartrack.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --init-box 100,80,180,220 \
  --output /mnt/sd/out_track.jpg
```

输出重点：

- `score`
- `box`

后处理重点：

- template patch 裁切
- search patch 裁切
- score map 峰值位置
- bbox map 回归框

### 6.12 车道线

推荐 spec：

- `configs/model_specs/your_lane_detection.mud`
- 模板：`configs/model_specs/template_lane_detection.mud`

命令：

```sh
./bin/tdl_lane_demo \
  --image /mnt/sd/test.jpg \
  --model-spec ./configs/model_specs/your_lane_detection.mud \
  --firmware ./firmware/libbm1688_kernel_module.so
```

输出重点：

- `lanes`
- `lane_state`
- 每条线的 `start / end / score`

说明：

- 当前 `LSTR_DET_LANE` 已切到自研 BMRT runtime
- 默认按 ImageNet 归一化处理；如果你的导出模型预处理不同，需要在 `.mud` 里显式改 `mean/scale`
- 当前结果结构仍是简化版，只输出每条 lane 的 `start/end/score`

### 6.13 语音活动检测

推荐 spec：

- `configs/model_specs/your_vad_fsmn.mud`
- 模板：`configs/model_specs/template_vad_fsmn.mud`

命令：

```sh
./bin/tdl_vad_demo \
  --pcm /mnt/sd/test_16k_mono.pcm \
  --model-spec ./configs/model_specs/your_vad_fsmn.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --chunk-bytes 3200
```

输出重点：

- `has_speech`
- `start_event`
- `end_event`
- `segments`

判读：

- `chunk-bytes` 用于流式测试
- 如果整段跑正常、分块跑异常，优先检查状态缓存逻辑而不是模型本身

## 7. 多阶段 demo

### 7.1 人脸检测 + 属性流水线

命令：

```sh
./bin/tdl_face_attr_pipeline_demo \
  --image /mnt/sd/test.jpg \
  --detector-model-spec ./configs/model_specs/scrfd_real.mud \
  --attribute-model-spec ./configs/model_specs/your_face_attribute.mud \
  --firmware ./firmware/libbm1688_kernel_module.so \
  --threshold 0.25 \
  --output /mnt/sd/out_face_attr_pipeline.jpg
```

用途：

- 验证多阶段 pipeline 是否正确传递 ROI
- 验证检测结果是否正确喂给属性分类器

## 8. 当前建议的主测 demo

如果只是做算法回归，建议优先使用下面这些主入口：

- 检测：`tdl_detect_demo`
- 人脸：`tdl_face_detect_demo`
- 分类：`tdl_classify_demo`
- 特征：`tdl_feature_demo`
- 人脸属性：`tdl_face_attribute_demo`
- 关键点：`tdl_keypoint_demo`
- 稠密人脸关键点：`tdl_face_dense_keypoint_demo`
- 实例分割：`tdl_instance_seg_demo`
- 语义分割：`tdl_semantic_seg_demo`
- 跟踪：`tdl_single_object_tracker_demo`
- 车道线：`tdl_lane_demo`
- VAD：`tdl_vad_demo`

## 9. 当前实现状态补充

截至当前分支：

- `Detector` / `FaceDetector` / `Classifier` / `FeatureExtractor` 已走自研 BMRT runtime
- `FaceAttributeClassifier` / `PlateRecognizer` 底层也已走自研 BMRT runtime
- `KeypointDetector` / `InstanceSegmenter` / `SemanticSegmenter` 是“自研 runtime + legacy fallback”并存
- `SingleObjectTracker` 已新增自研 BMRT 原型
- `LaneDetector` / `VoiceActivityDetector` 仍主要走 vendor 路径

这意味着测试时如果发现异常，需要先区分当前算法到底走的是：

- 自研 BMRT runtime
- 还是 vendor legacy runtime

最简单的方法是：

- 看终端里是否打印了我们新增的 debug 标记
- 看二进制 `strings` 是否包含对应调试字符串
- 对照当前 `model_type` 是否已经在自研路径覆盖范围内
