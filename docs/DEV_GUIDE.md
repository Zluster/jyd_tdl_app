# 开发指南

本文给出最常见的三类开发问题：

1. 应该选哪个 API
2. 新增一个算法 wrapper 时，后处理写在哪里
3. 一台新板子从 clone 到编译、相机、音频、算法验证的完整步骤

## 一、按需求选择 API

| 需求 | 推荐 API | 说明 |
| --- | --- | --- |
| 单张图片检测 | `Detector` | 最简单，直接传图片路径 |
| 相机原生帧检测 | `Detector + VIDEO_FRAME_INFO_S` | 当前推荐路径 |
| 单张图片分类 | `Classifier` | 输出类别与分数 |
| 单张图片提特征 | `FeatureExtractor` | 输出 embedding |
| 两阶段或多阶段流程 | `MultiStagePipeline` | 例如先检测再属性分类 |
| 只想取相机帧 | `Camera` | 输出 `Frame` |
| 想自己控制媒体图 | `advanced.hpp` 中的媒体类 | 例如 `Mmf`、`VpssGroup`、`VencChannel` |

## 二、Detector 现在推荐怎么写

### 1. 单张图片

```cpp
std::string error;
tdl_app::Detector detector(
    tdl_app::ModelSessionConfig::fromSpec(
        "./configs/model_specs/yolov5s_det_coco80.ini",
        "./firmware/libbm1688_kernel_module.so",
        "./models",
        "YOLOV5"),
    &error);

tdl_app::InferOptions options = tdl_app::InferOptions::detection(0.25f, 0.45f);
tdl_app::AlgorithmResult result = detector("./test.jpg", options, &error);
```

### 2. 相机原生帧

```cpp
VIDEO_FRAME_INFO_S frame = ...;
tdl_app::AlgorithmResult result = detector(frame, options, &error);
```

### 3. 兼容旧写法

仍然可以：

```cpp
detector.load(config, &error);
detector.run(path, options, &result, &error);
detector.runFrame(frame, options, &result, &error);
```

## 三、新增算法 wrapper 时，后处理写在哪里

原则很明确：

- 后处理写在对应运行时类内部
- 不要散落到 demo
- 不要让业务层自己做 NMS、DFL 解码、CTC decode

推荐模板：

```cpp
class NnYourAlgo : public NnBase {
 public:
  bool load(EngineConfig config, std::string *error) override;
  bool predict(const std::string &image_path, const InferOptions &options,
               AlgorithmResult *result, std::string *error) override;
  bool predictFrame(const Frame &frame, const InferOptions &options,
                    AlgorithmResult *result, std::string *error) override;

 private:
  void preprocess(...);
  bool launch(...);
  void decode(...);
};
```

建议分工：

- `load/open`：模型和 runtime 初始化
- `predict/predictFrame`：单次推理主流程
- `preprocess`：图像到 tensor
- `launch`：runtime 执行
- `decode/postprocess`：后处理和结果填充

## 四、完整操作：新板子从 clone 到验证

### 1. 安装通用依赖

```sh
apk add opencv-dev tinyalsa-dev zlib-dev
```

### 2. clone 仓库

```sh
git clone https://github.com/Zluster/jyd_tdl_app.git /mnt/git/jyd_tdl_app
cd /mnt/git/jyd_tdl_app
```

### 3. 初始化板端环境

```sh
sh scripts/setup_board_native_env.sh
```

### 4. 原生配置与编译

```sh
cmake -S /mnt/git/jyd_tdl_app \
  -B /mnt/sd/tdl_build_sys \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /mnt/sd/tdl_build_sys --target tdl_classify_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_detect_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_camera_capture_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_audio_aio_demo -j1
```

### 5. 加载运行环境

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

### 6. 运行分类验证

```sh
/mnt/sd/tdl_build_sys/tdl_classify_demo \
  --image /mnt/sd/dog.jpg \
  --model-spec /mnt/git/jyd_tdl_app/configs/model_specs/cls_hand_gesture.ini \
  --firmware /mnt/git/jyd_tdl_app/firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/dog_cls_syslibs.jpg
```

### 7. 运行相机抓图验证

```sh
/mnt/sd/tdl_build_sys/tdl_camera_capture_demo \
  --use-sensor-media \
  --sensor-ini /mnt/git/jyd_tdl_app/configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini \
  --backend vpss \
  --width 1920 \
  --height 1080 \
  --timeout-ms 4000 \
  --frames 1 \
  --output /mnt/sd/fullstack_probe.jpg
```

### 8. 运行音频录制和播放验证

录音：

```sh
/mnt/sd/tdl_build_sys/tdl_audio_aio_demo \
  --mode record \
  --seconds 5 \
  --ai-volume 24 \
  --output /mnt/sd/test_ai_16k_mono.pcm
```

播放：

```sh
/mnt/sd/tdl_build_sys/tdl_audio_aio_demo \
  --mode play \
  --input /mnt/sd/test_ai_16k_mono.pcm \
  --ao-volume 24
```

## 五、常见问题

### 1. `libtdl_core.so: No such file or directory`

说明没有加载板端运行环境，执行：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

### 2. `/tmp/firmwareXXXX.so: Exec format error`

优先检查：

- `BMRUNTIME_USING_FIRMWARE` 是否已设置
- 仓库中的 firmware 是否可用

### 3. `linux/fb.h: No such file or directory`

仓库已经提供兼容头，重新 `cmake` 和 `build` 即可。

### 4. 相机抓不到图

先确认：

- 是否使用了 `sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
- 是否使用了 `--backend vpss`

当前已验证：问题核心来自 ini 配置不匹配，而不是相机 demo 本身的读帧逻辑。
