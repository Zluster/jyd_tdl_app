# tdl_app_sdk MMF 说明

本文是根目录 MMF 文档，描述媒体图在项目中的职责。

## 当前最重要的认知

- `Frame.native` 往往可以直接拿到 `VIDEO_FRAME_INFO_S`
- `Detector` 已支持直接处理 `VIDEO_FRAME_INFO_S`
- 相机输出如果已经是 RGB、BGR、NV12、NV21，就不需要先转成 JPG 再推理

## 典型媒体图

### 相机抓图

```text
sensor -> MIPI -> ISP -> VI -> VPSS -> 抓图保存
```

### 相机检测

```text
sensor -> MIPI -> ISP -> VI / VPSS -> VIDEO_FRAME_INFO_S -> Detector
```

### 推流

```text
sensor / VPSS -> VENC -> RTSP
```

## 当前板端验证结果

- 相机链路已经确认可工作
- 之前失败核心是 ini 配置不匹配
- 当前板子上推荐使用：
  - `sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
  - `--backend vpss`
