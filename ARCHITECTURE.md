# tdl_app_sdk 架构说明

本文是根目录架构文档，描述项目整体组成和职责划分。

## 设计目标

- 提供稳定、可复用的上层 C++ API
- 收敛厂商 SDK 细节
- 支持图片、相机原生帧和板端媒体图输入
- 支持 SDK 主机交叉编译和板端原生编译
- 保持算法、媒体、输出模块解耦

## 主要模块

### 1. 公共 API 层

主要位于：

- `include/tdl_app/`

对外典型类包括：

- `Detector`
- `Classifier`
- `FaceDetector`
- `FeatureExtractor`
- `Camera`
- `Pipeline`

### 2. 算法实现层

主要位于：

- `src/algorithm/`

负责：

- 模型加载
- 预处理
- 推理调用
- 后处理

### 3. 媒体实现层

主要位于：

- `src/media/`
- `src/media/private/`

负责：

- sensor / MIPI / ISP / VI / VPSS
- OSD、显示、编码、RTSP
- `Frame` 与相机链路封装

### 4. 平台依赖层

- CV184X 专用依赖：`third_party/cv184x/lib`
- 系统通用依赖：`OpenCV`、`zlib`、`tinyalsa`

## 当前重要结论

- 板端原生编译已经可工作
- 通用库已改用系统库
- 相机抓图当前已验证使用：
  - `sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
  - `--backend vpss`
