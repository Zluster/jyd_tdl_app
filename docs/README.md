# 文档总览

这里集中维护 `tdl_app_sdk` 的设计、开发、媒体、板端运行和接口文档。

## 建议阅读顺序

1. [项目总览](../README.md)
2. [架构说明](ARCHITECTURE.md)
3. [算法说明](ALGORITHMS.md)
4. [开发指南](DEV_GUIDE.md)
5. [MMF 与媒体图](MMF.md)
6. [绑定设计](BINDINGS.md)
7. [双核媒体控制](DUAL_CORE.md)
8. [API 与 demo 对照](ALG_INFO.md)
9. [板端原生编译与测试指南](board_native_build_guide_zh.md)

## 按需求查文档

- 想快速跑通一台新板子：
  - 看 [板端原生编译与测试指南](board_native_build_guide_zh.md)
- 想知道有哪些算法封装：
  - 看 [算法说明](ALGORITHMS.md)
- 想知道 API 如何使用：
  - 看 [开发指南](DEV_GUIDE.md)
- 想知道 API 与 demo 的映射关系：
  - 看 [API 与 demo 对照](ALG_INFO.md)
- 想知道媒体图、VI、VPSS、RTSP、OSD 的分工：
  - 看 [MMF 与媒体图](MMF.md)
- 想知道为什么 API 要这么设计：
  - 看 [绑定设计](BINDINGS.md) 和 [架构说明](ARCHITECTURE.md)

## 当前重点结论

- `Detector` 现在支持直接吃 `VIDEO_FRAME_INFO_S`。
- `YOLOv5` 已支持 RGB、BGR、NV12、NV21、YUV400 等原生帧格式。
- 板端原生编译已切到系统 `OpenCV`、`zlib`、`tinyalsa`。
- 平台专用库仍然继续使用 `third_party/cv184x/lib`。
- 相机抓图当前推荐使用：
  - `configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
  - `--backend vpss`
