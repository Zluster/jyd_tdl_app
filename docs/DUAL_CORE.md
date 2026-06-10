# 双核媒体控制

本文说明在 CVITEK 双核场景下，`tdl_app_sdk` 建议如何分工。

## 推荐分工

- Linux 大核负责真正的媒体执行路径
- 小核 AliOS / C906 负责控制、UI、状态机
- 两边通过 IPC 通信

## 为什么 `CVI_MPI` 不适合搬到小核

因为真正的 `CVI_MPI` 运行依赖：

- Linux 驱动
- 设备节点和 `ioctl`
- `SYS/VB`
- MMZ / ION / cache 操作
- `VI/VPSS/VENC/VDEC/VO/RGN` 驱动绑定

所以正确方案不是“把整套媒体栈搬到小核”，而是：

- 媒体执行留在 Linux
- 小核通过项目自定义协议控制 Linux 服务

## Linux 侧负责什么

- `SYS`
- `VB`
- `VI`
- `VPSS`
- `VO`
- `RGN/OSD`
- `VENC`
- `VDEC`
- sensor / MIPI / ISP
- RTSP
- TDL 推理

## 小核侧负责什么

- UI
- 控制状态机
- GPIO / 按键
- 轻量业务逻辑
- 向 Linux 发送媒体控制请求

## 工程里的对应方向

当前建议新增和保持的层次：

- `media_ipc_protocol.hpp`：项目自有协议
- `media_ipc_client.hpp`：小核或远端调用侧
- `media_ipc_server.hpp`：Linux 服务侧

这样以后即使底层厂商 IPC 细节变了，对外协议也能保持稳定。
