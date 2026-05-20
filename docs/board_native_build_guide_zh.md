# 板端原生编译与测试指南

本文档说明如何在一块新的 CVITEK 板子上，从 `git clone` 开始完成：

- 板端原生编译
- 使用系统通用库，而不是仓库内 bundle 的 OpenCV、zlib、tinyalsa
- 运行纯算法 demo
- 排查常见环境问题

## 1. 当前依赖策略

当前仓库采用以下规则：

- 平台专用库继续使用 `third_party/cv184x/lib`
  - 例如 `libtdl_core.so`、`libbmrt.so`、`libbmlib.so`、`libsys.so`
- 通用库改用系统库
  - `OpenCV`
  - `zlib`
  - `tinyalsa`
- 板端运行算法 demo 时，必须提供可用的 `firmware/libbm1688_kernel_module.so`

说明：

- `LD_LIBRARY_PATH` 用来优先找到 vendor 平台库。
- `BMRUNTIME_USING_FIRMWARE` 用来让 BMRuntime 使用正确的 TPU firmware。
- `--firmware` 参数建议继续保留，但在板端环境里，单独传参通常不如环境变量稳定。

## 2. 新板子第一次准备

假设仓库放在：

```sh
/mnt/git/jyd_tdl_app
```

进入仓库根目录：

```sh
cd /mnt/git/jyd_tdl_app
```

执行一次初始化脚本：

```sh
sh scripts/setup_board_native_env.sh
```

这个脚本会做三件事：

1. 创建 `models` 软链接

```sh
models -> third_party/cv184x/models
```

2. 创建 `firmware` 软链接

```sh
firmware -> third_party/cv184x/firmware
```

3. 补系统 OpenCV 兼容软链接

脚本会尝试创建：

```sh
/usr/lib/libopencv_core.so.4.5
/usr/lib/libopencv_imgproc.so.4.5
/usr/lib/libopencv_imgcodecs.so.4.5
```

## 3. 系统库要求

板端原生编译和运行时需要系统已经提供：

- OpenCV 头文件和库
- `zlib`
- `tinyalsa`

建议先确认：

```sh
ls -ld /usr/include/opencv4
ls -l /usr/include/zlib.h
ls -ld /usr/include/tinyalsa
ls -l /usr/lib/libopencv_core.so*
ls -l /usr/lib/libopencv_imgproc.so*
ls -l /usr/lib/libopencv_imgcodecs.so*
ls -l /usr/lib/libtinyalsa.so*
ls -l /lib/libz.so*
```

如果新板子缺少这些系统库，可以先安装：

```sh
apk add opencv-dev tinyalsa-dev zlib-dev
```

## 4. 板端原生编译

推荐使用源码外构建目录：

```sh
cmake -S /mnt/git/jyd_tdl_app \
  -B /mnt/sd/tdl_build_sys \
  -DCMAKE_BUILD_TYPE=Release
```

编译常用 demo：

```sh
cmake --build /mnt/sd/tdl_build_sys --target tdl_classify_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_detect_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_camera_capture_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_audio_aio_demo -j1
```

## 5. 运行前环境

方式 A：source 仓库根目录 `env.sh`

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

方式 B：直接用包装脚本执行

```sh
sh scripts/run_with_board_env.sh /mnt/sd/tdl_build_sys/tdl_classify_demo ...
```

## 6. 纯算法 demo 测试命令

分类 demo：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh

/mnt/sd/tdl_build_sys/tdl_classify_demo \
  --image /mnt/sd/dog.jpg \
  --model-spec /mnt/git/jyd_tdl_app/configs/model_specs/cls_hand_gesture.ini \
  --firmware /mnt/git/jyd_tdl_app/firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/dog_cls_syslibs.jpg
```

检测 demo：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh

/mnt/sd/tdl_build_sys/tdl_detect_demo \
  --image /mnt/sd/dog.jpg \
  --model-spec /mnt/git/jyd_tdl_app/configs/model_specs/yolov5s_det_coco80.ini \
  --firmware /mnt/git/jyd_tdl_app/firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/dog_det_syslibs.jpg
```

## 7. 相机抓图测试

当前已验证可工作的配置是：

- sensor ini：`configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini`
- backend：`vpss`

测试命令：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh

/mnt/sd/tdl_build_sys/tdl_camera_capture_demo \
  --use-sensor-media \
  --sensor-ini /mnt/git/jyd_tdl_app/configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini \
  --backend vpss \
  --width 1920 \
  --height 1080 \
  --timeout-ms 4000 \
  --frames 1 \
  --output /mnt/sd/fullstack_probe.jpg
```

说明：

- 之前抓不到图，核心不是代码读帧流程有问题，而是 ini 配置不匹配。
- 换成 ipcamera 使用的 ini 后，VI/VPSS 已经能稳定收帧。
- 当前这块板子上，`--backend vi` 仍然会超时；`--backend vpss` 已验证可用。

## 8. 音频 AIO 基本测试

录音：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh

/mnt/sd/tdl_build_sys/tdl_audio_aio_demo \
  --mode record \
  --seconds 5 \
  --ai-volume 24 \
  --output /mnt/sd/test_ai_16k_mono.pcm
```

播放：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh

/mnt/sd/tdl_build_sys/tdl_audio_aio_demo \
  --mode play \
  --input /mnt/sd/test_ai_16k_mono.pcm \
  --ao-volume 24
```

如果文件大小符合预期但仍然听不到声音，优先检查：

- 板端扬声器/功放硬件路径是否实际接通
- DAC 输出音量是否被系统其它流程覆盖
- 当前板卡音频通道是否确实连到外放，而不是耳机或其它路径

## 9. 如何确认确实用了系统通用库

先看构建阶段的 `CMakeCache.txt`：

```sh
grep -E '^OpenCV_DIR:|^TDL_TINYALSA_LIBRARY:|^ZLIB_' /mnt/sd/tdl_build_sys/CMakeCache.txt
```

再看运行时依赖：

```sh
ldd /mnt/sd/tdl_build_sys/tdl_classify_demo
readelf -d /mnt/git/jyd_tdl_app/third_party/cv184x/lib/libtdl_core.so | grep NEEDED
```

判断原则：

- `libtdl_core.so`、`libbmrt.so`、`libbmlib.so` 这类平台库，仍然来自 `third_party/cv184x/lib`
- `libopencv_*`、`libz.so`、`libtinyalsa.so` 应该来自系统目录

## 10. 常见问题

### 10.1 `libtdl_core.so: No such file or directory`

处理：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

### 10.2 `/tmp/firmwareXXXX.so: Exec format error`

处理顺序：

1. 确认环境变量：

```sh
echo $BMRUNTIME_USING_FIRMWARE
```

2. 确认 firmware 文件存在：

```sh
ls -l /mnt/git/jyd_tdl_app/third_party/cv184x/firmware/libbm1688_kernel_module.so
```

3. 如仍失败，用已验证可运行的 firmware 覆盖仓库内 firmware：

```sh
cp -f \
  /mnt/sd/tdl_app_sdk_cv184x/firmware/libbm1688_kernel_module.so \
  /mnt/git/jyd_tdl_app/third_party/cv184x/firmware/libbm1688_kernel_module.so
```

### 10.3 `failed to open bmodel`

处理：

```sh
cd /mnt/git/jyd_tdl_app
ln -sfn third_party/cv184x/models models
```

### 10.4 `libopencv_*.so.4.5` 找不到

处理：

```sh
sh scripts/setup_board_native_env.sh
```

### 10.5 `linux/fb.h: No such file or directory`

当前仓库已经内置兼容头，重新配置并编译即可：

```sh
cmake -S /mnt/git/jyd_tdl_app \
  -B /mnt/sd/tdl_build_sys \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /mnt/sd/tdl_build_sys --target tdl_classify_demo -j1
```

## 11. 新设备从 clone 到编译测试的完整步骤

1. clone 仓库：

```sh
git clone https://github.com/Zluster/jyd_tdl_app.git /mnt/git/jyd_tdl_app
cd /mnt/git/jyd_tdl_app
```

2. 如系统尚未安装通用依赖，先安装：

```sh
apk add opencv-dev tinyalsa-dev zlib-dev
```

3. 执行板端初始化：

```sh
sh scripts/setup_board_native_env.sh
```

4. 配置并编译：

```sh
cmake -S /mnt/git/jyd_tdl_app \
  -B /mnt/sd/tdl_build_sys \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /mnt/sd/tdl_build_sys --target tdl_classify_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_detect_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_camera_capture_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_audio_aio_demo -j1
```

5. 加载运行环境：

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

6. 运行分类测试：

```sh
/mnt/sd/tdl_build_sys/tdl_classify_demo \
  --image /mnt/sd/dog.jpg \
  --model-spec /mnt/git/jyd_tdl_app/configs/model_specs/cls_hand_gesture.ini \
  --firmware /mnt/git/jyd_tdl_app/firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/dog_cls_syslibs.jpg
```

7. 运行抓图测试：

```sh
/mnt/sd/tdl_build_sys/tdl_camera_capture_demo \
  --use-sensor-media \
  --sensor-ini /mnt/git/jyd_tdl_app/configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini \
  --backend vpss \
  --width 1920 \
  --height 1080 \
  --timeout-ms 4000 \
  --frames 1 \
  --output /mnt/sd/fullstack_probe.jpg
```

做到这里，就已经具备一台新板子从 clone 到编译、算法测试、相机测试的完整闭环。
