# jyd_tdl_app

`jyd_tdl_app` 现在只面向 `CV184X 双系统` 场景。

这里的约束是明确的：

- 大核应用只走双系统 MMF 通路
- 小核负责底层 `VI/VPSS/VO/RGN/音频` 初始化与运行
- 大核通过 `CVI_MSG` 访问媒体能力，不再兼容单系统直连方案
- 依赖目录固定为 `third_party/cv184x/dual_os`

如果你看到旧的 `third_party/cv184x/lib`、`single_os`、`sensor_media 直起整套媒体图` 一类说法，以当前仓库为准，统一按双系统理解。

## 文档入口

- [总体架构](./docs/ARCHITECTURE.md)
- [MMF 数据流与接口说明](./docs/MMF.md)
- [编译、打包、上板测试指南](./docs/board_native_build_guide_zh.md)

## 当前推荐目录约定

```text
jyd_tdl_app/
  apps/                         demo 与测试程序
  configs/                      模型与传感器配置
  docs/                         中文设计与使用文档
  include/tdl_app/              对外头文件
  scripts/                      构建、打包、板端运行脚本
  src/algorithm/                算法封装
  src/media/                    媒体与双系统 MMF 封装
  third_party/cv184x/dual_os/   双系统依赖与运行库
```

## 当前推荐开发方式

1. 小核先正常启动 MMF 服务
2. 大核运行 `jyd_tdl_app` demo 或业务程序
3. 大核只从固定媒体源取流，例如 `live / ai / main / subrgb`
4. 算法层尽量只关心 `Frame`、`VIDEO_FRAME_INFO_S`、结果结构体

## 关键原则

- 不再把 `jyd_tdl_app` 设计成单系统/双系统共用工程
- 不再让打包脚本从模糊路径回退拷库
- 大核 demo 要尽量对齐 `sophpi_mmf_ctl` 的使用习惯
- 板端测试优先验证“包内库是否为 dual_os 版本”

## 最短验证路径

在 SDK 主机上：

```sh
cd /home/jyd/zwz/sophpi/jyd_tdl_app
ssh-keygen -f '/home/aixtr/.ssh/known_hosts' -R '10.0.0.1'
export TDL_APP_PROFILE=dual_os
export TDL_APP_BUILD_KWS=OFF
export TDL_APP_THIRD_PARTY_DIR=/home/aixtr/jyd/jyd_tdl_app/third_party/cv184x
export TOOLCHAIN_ROOT=/home/aixtr/jyd/sophpi/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf
./scripts/build_cv184x.sh && ./scripts/package_runtime.sh && scp package/tdl_app_sdk_cv184x.tar.gz root@10.0.0.1:/root
```

在板子上：

```sh
cd /mnt/sd/tdl_app_sdk_cv184x
gzip -dc tdl_app_sdk_cv184x.tar.gz | tar -xf -
. ./env.sh
./run_camera_capture_demo.sh --source live --output live.jpg
./run_camera_capture_demo.sh --source ai --output ai.jpg
```
