# MMF 与媒体图

本文说明当前项目里的媒体图职责划分，以及应用层最需要关心的几个点。

## 你最需要先记住的结论

- `Camera` 取出来的 `Frame.native` 往往就是 `VIDEO_FRAME_INFO_S*`
- `Detector` 现在可以直接吃 `VIDEO_FRAME_INFO_S`
- 如果相机已经输出 RGB、BGR 或 NV12、NV21，就不需要先转 JPG 再推理
- 相机抓图当前已验证可用路径是：
  - `sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
  - `--backend vpss`

## 媒体相关主要对象

- `Mmf`
- `SensorMedia`
- `Camera`
- `Frame`
- `FrameSource`
- `FrameSink`
- `RtspStreamer`
- `OsdRegion`
- `GraphicVoLayer`
- `VencChannel`
- `VdecChannel`

## 典型媒体图

### 相机抓图

```text
sensor
  -> MIPI
  -> ISP
  -> VI
  -> VPSS
  -> Frame
  -> 保存图片
```

### 相机检测

```text
sensor
  -> MIPI
  -> ISP
  -> VI / VPSS
  -> VIDEO_FRAME_INFO_S
  -> Detector
  -> AlgorithmResult
```

### RTSP 推流

```text
sensor / VPSS
  -> VENC
  -> RtspStreamer
```

## 当前板端相机验证结论

本轮板端调试已确认：

- 相机链路本身可以出图
- 之前失败核心原因是 ini 配置不匹配
- 改为 `sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini` 后，相机初始化和出帧恢复正常
- 当前板子上 `--backend vi` 仍可能超时
- 当前板子上 `--backend vpss` 已验证可工作
