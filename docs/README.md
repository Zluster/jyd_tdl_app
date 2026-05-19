# 文档总览

这里集中放 `tdl_app_sdk` 的设计、接口、开发和算法文档。

## 推荐阅读顺序

1. [README](../README.md)
2. [架构说明](ARCHITECTURE.md)
3. [算法说明](ALGORITHMS.md)
4. [开发指南](DEV_GUIDE.md)
5. [MMF 与媒体图](MMF.md)
6. [绑定设计](BINDINGS.md)
7. [双核控制](DUAL_CORE.md)
8. [API 与 demo 对照](ALG_INFO.md)

## 当前接口重点

- `Detector` 支持构造即加载
- `Detector` 支持直接接收 `VIDEO_FRAME_INFO_S`
- `YOLOv5` 支持 RGB/BGR/NV12/NV21/YUV400 原生帧
- 同时兼容图片路径输入和相机输入

## 当前最常用文档

- 想看“怎么调用”：看 [开发指南](DEV_GUIDE.md)
- 想看“有哪些算法”：看 [算法说明](ALGORITHMS.md)
- 想看“API 和 demo 对应关系”：看 [API 与 demo 对照](ALG_INFO.md)
- 想看“为什么这样设计”：看 [架构说明](ARCHITECTURE.md)
