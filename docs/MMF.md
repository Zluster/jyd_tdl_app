# MMF 与媒体图

详细版本见：

- [根目录 MMF 文档](../MMF.md)

## 当前你最需要关心的点

- `Camera` 取出的 `Frame.native` 往往就是 `VIDEO_FRAME_INFO_S*`
- `YOLOv5` 现在可以直接吃这个原生帧
- 如果相机已经输出 RGB/BGR，就不必业务层再手动转图片
