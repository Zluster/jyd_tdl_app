# 开发指南

本文给出两类最常见开发问题：

1. 从需求选择 API
2. 自己增加一个新算法 wrapper 时，后处理写在哪里、怎么写

## 从需求选 API

| 需求 | 推荐 API | 说明 |
| --- | --- | --- |
| 单张图片做检测 | `Detector` | 最简单，直接传图片路径 |
| 相机原生帧做检测 | `Detector` + `VIDEO_FRAME_INFO_S` | 现在可以直接传原生帧 |
| 单张图片做分类 | `Classifier` | 输出类别和分数 |
| 单张图片提特征 | `FeatureExtractor` | 输出 embedding |
| 两阶段流程 | `MultiStagePipeline` | 例如先检测再属性分类 |
| 只想取相机帧 | `Camera` | 输出 `Frame` |
| 想自己控制媒体图 | `advanced.hpp` 里的媒体类 | 例如 `Mmf`、`VpssGroup`、`VencChannel` |

## Detector 现在推荐怎么写

### 单张图片

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

### 相机原生帧

```cpp
VIDEO_FRAME_INFO_S frame = ...;
tdl_app::AlgorithmResult result = detector(frame, options, &error);
```

### 兼容旧写法

仍然可以：

```cpp
detector.load(config, &error);
detector.run(path, options, &result, &error);
detector.runFrame(frame, options, &result, &error);
```

## YOLOv5 一次推理时序

### open()

负责一次性初始化：

1. `bm_dev_request`
2. 需要时设置 firmware 环境变量
3. `bmrt_create`
4. `bmrt_load_bmodel`
5. 读取网络名和网络信息
6. 解析输入尺寸、输入 dtype、输出个数
7. 解析 anchor 和标签

### preprocess()

负责单帧预处理：

1. 统一得到 `cv::Mat`
2. letterbox 到模型输入尺寸
3. `BGR -> RGB`
4. 转 float
5. 排成网络输入 tensor 顺序

### launch()

负责单帧推理：

1. 按输入 dtype 做量化或直接传 float
2. 分配输出 buffer
3. 调用 `bmrt_launch_data`
4. 把输出转成 float 向量

### decode()

负责后处理：

1. 按输出布局解析网格
2. 结合 anchor 解码框
3. 计算目标分数
4. 回映射到原图
5. 执行 NMS
6. 写入 `AlgorithmResult.boxes`

## 自己加一个新算法 wrapper，后处理写在哪

原则：

- 后处理写在对应运行时类内部
- 不要写到 demo
- 不要散落到 `Detector` 外层
- 对外只暴露稳定结果结构

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

### 建议分工

- `load/open`：模型和 runtime 初始化
- `predict/predictFrame`：单次推理主流程
- `preprocess`：图像到 tensor
- `launch`：runtime 执行
- `decode`：后处理解码和结果填充

## 新算法 wrapper 模板

### 第一步：确定结果类型

如果最终还是框、分类、属性、文本中的一种，优先复用已有结构：

- `AlgorithmResult`
- `KeypointResult`
- `SemanticSegmentationResult`
- `InstanceSegmentationResult`
- `LaneDetectionResult`

### 第二步：处理输入

优先同时支持：

- 图片路径
- `Frame`
- 如果是检测类，尽量支持 `VIDEO_FRAME_INFO_S`

### 第三步：后处理落点

后处理必须放在 runtime 类内，常见是：

- `decode(...)`
- 或 `postprocess(...)`

不要把以下逻辑写在 demo：

- NMS
- softmax
- anchor 解码
- DFL 解码
- CTC decode
- landmark 映射

### 第四步：对外接口

优先让上层能这样写：

```cpp
tdl_app::YourWrapper algo(config, &error);
auto result = algo(input, options, &error);
```

## 关于相机流性能

相机流最忌讳的是：

- 每帧构造 detector
- 每帧 `load` 模型
- 每帧重复做无意义格式转换

正确做法：

1. 启动时构造并 `load`
2. 运行期只重复调用推理
3. 如果相机能直接输出 RGB/BGR，直接走原生帧路径

## 关于多模型并行

多模型场景本身不会天然异常，但要注意：

- 每个模型实例独占自己的 runtime
- 不要多个线程无保护共享同一个实例
- 要控制总带宽、总内存和推理排队关系
