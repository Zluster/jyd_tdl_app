# 架构说明

详细架构说明与仓库根目录版本一致，这里保留文档入口方便直接跳转阅读。

建议直接阅读：

- [根目录架构说明](../ARCHITECTURE.md)

## 关键结论

- 媒体栈和算法栈分离
- 通过 `Frame / FrameSource / FrameSink` 对接
- 算法内部统一负责预处理、推理、后处理
- 上层优先使用稳定包装类，例如 `Detector`、`Classifier`、`Camera`
