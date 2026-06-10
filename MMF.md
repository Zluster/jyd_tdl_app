# MMF 与媒体图

本工程把 CVITEK 板端媒体初始化分成两层：

- 细粒度组件层
- 便捷组装层

## 细粒度组件层

这层直接对应 `SYS/VB/VI/VPSS/VENC/VDEC/VO/RGN`：

- `MediaSystem`
- `SysContext`
- `VideoBufferManager`
- `VideoBufferPool`
- `ViChannel`
- `VpssGroup`
- `MediaLink`
- `FrameReader`
- `VencChannel`
- `VdecChannel`
- `VoOutput`
- `OsdRegion`
- `GraphicVoLayer`

适合：

- 自定义媒体图
- 多路 VPSS
- 一路输入多路输出
- 编码 / 预览 / OSD 同时存在

## 便捷组装层

这层用于快速拉起一套可用链路：

- `Mmf`
- `SensorMedia`
- `Camera`

适合：

- 相机取流 demo
- 算法验证
- 常规单路预览或推理

## Camera 的定位

`Camera` 本质上是对 `FrameReader` 的高层包装。它负责：

- 打开媒体图
- 取一帧
- 回收资源

上层拿到的是：

- `Frame`

如果底层是相机原生帧，`Frame.native` 通常就是：

- `VIDEO_FRAME_INFO_S*`

## 与算法的连接点

媒体栈和算法栈的连接点是：

- `Frame`
- `FrameSource`

这意味着：

- 单张图可以走 `image_path`
- 相机帧可以走 `VIDEO_FRAME_INFO_S`
- 算法层不需要知道媒体图是怎么搭出来的

## 当前 YOLOv5 的相机帧路径

`YOLOv5` 现在已经支持直接接收 `VIDEO_FRAME_INFO_S`。

支持格式：

- `RGB888`
- `BGR888`
- `RGB888_PLANAR`
- `BGR888_PLANAR`
- `NV12`
- `NV21`
- `YUV400`

算法内部会按格式转成 OpenCV `BGR Mat`，再统一做 letterbox 和推理。

## 性能建议

如果摄像头能直接输出 RGB/BGR：

- 优先直接给算法
- 避免先落 JPG 再读回
- 避免业务层自己重复做格式转换

如果是持续视频流：

- 不要每帧重建 `Detector`
- 不要每帧重新 `load` 模型
- 让同一个 detector 长时间复用

## 多模型建议

多模型同时运行时：

- 每个模型实例单独持有自己的 runtime
- 初始化阶段分别 `load`
- 帧级调度由业务层控制

不要做的事情：

- 多线程同时共享一个未做并发保护的 detector 实例
- 每帧反复创建 / 销毁模型实例
