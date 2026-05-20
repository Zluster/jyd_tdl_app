# 绑定设计

本文说明为什么当前公开 API 要尽量保持“适合做 Python、MicroPython 或其它脚本绑定”的形状。

## 当前绑定友好原则

公开 API 尽量满足以下规则：

- 配置和结果优先使用 plain struct
- 生命周期清晰
- 默认路径和所有权关系简单
- 尽量不向外抛异常
- 优先使用 `bool + std::string* error`

## 适合优先暴露的接口

- `Detector`
- `Classifier`
- `FaceDetector`
- `FaceAttributeClassifier`
- `PlateRecognizer`
- `FeatureExtractor`
- `Camera`
- `Pipeline`
- `MultiStagePipeline`
- `ModelSessionConfig`
- `InferOptions`
- `AlgorithmResult`

## 为什么要这样设计

脚本绑定最怕三类问题：

1. 隐式生命周期
2. 复杂模板或深层继承暴露到绑定层
3. 厂商原生大结构直接外漏

因此公开层要尽量更平、更稳定，让绑定层只处理少量高层对象和结果结构。

## 新接口的绑定优势

当前 `Detector` 推荐写法：

```cpp
tdl_app::Detector detector(config, &error);
tdl_app::AlgorithmResult result = detector("./a.jpg", options, &error);
```

如果是相机原生帧：

```cpp
tdl_app::AlgorithmResult result = detector(video_frame, options, &error);
```

对绑定层来说，这比“先造 `Frame` 再调用 `runFrame`”更直接。

## 哪些接口不建议作为默认脚本 API

以下接口更适合保留在高级媒体层：

- `VencChannel`
- `VdecChannel`
- `ViChannel`
- `VpssGroup`
- `VideoBufferPool`
- `SysContext`
- `MediaLink`

原因不是它们没用，而是：

- 设备资源生命周期更复杂
- 对平台依赖更强
- 更容易出现脚本层误用

因此更适合归在：

- `advanced.hpp`

## 新增接口时的建议

- 如果上层最常见的使用路径能压成“构造 + 调用”，优先这么设计。
- 如果结果能复用 `AlgorithmResult`，优先复用。
- 如果一个接口必须暴露硬件句柄或复杂资源链路，就不要把它放在默认公共 API 层。
