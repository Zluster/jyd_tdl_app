# 绑定设计

这个工程的对外 API 有意保持“容易做 Python / MicroPython 绑定”的形状。

## 当前绑定友好规则

公开 API 尽量满足：

- 配置和结果使用 plain struct
- 生命周期清晰
- 默认路径不玩复杂所有权
- 不抛异常
- 用 `bool + std::string* error`

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

因为脚本绑定最怕三种东西：

1. 隐式生命周期
2. 很重的模板和继承暴露
3. 一堆厂商原生大结构体直接外泄

所以当前高层 API 被限制成更平的形状。

## 新接口的绑定优势

本轮新增后，`Detector` 的调用更适合绑定：

```cpp
tdl_app::Detector detector(config, &error);
tdl_app::AlgorithmResult result = detector("./a.jpg", options, &error);
```

如果是相机原生帧：

```cpp
tdl_app::AlgorithmResult result = detector(video_frame, options, &error);
```

对绑定层来说，这比“先拼 `Frame` 再调 `runFrame`”更直接。

## 哪些仍然建议放到高级接口

以下内容不建议马上做成默认脚本 API：

- `VencChannel`
- `VdecChannel`
- `ViChannel`
- `VpssGroup`
- `VideoBufferPool`
- `SysContext`
- `MediaLink`

它们更适合保留在：

- `advanced.hpp`
