# 开发指南（简版）

如果你只想快速知道“怎么编译、怎么跑”，按下面顺序做：

1. 安装系统依赖

```sh
apk add opencv-dev tinyalsa-dev zlib-dev
```

2. clone 仓库并初始化

```sh
git clone https://github.com/Zluster/jyd_tdl_app.git /mnt/git/jyd_tdl_app
cd /mnt/git/jyd_tdl_app
sh scripts/setup_board_native_env.sh
```

3. 板端原生编译

```sh
cmake -S /mnt/git/jyd_tdl_app \
  -B /mnt/sd/tdl_build_sys \
  -DCMAKE_BUILD_TYPE=Release

cmake --build /mnt/sd/tdl_build_sys --target tdl_classify_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_detect_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_camera_capture_demo -j1
cmake --build /mnt/sd/tdl_build_sys --target tdl_audio_aio_demo -j1
```

4. 加载环境

```sh
cd /mnt/git/jyd_tdl_app
. ./env.sh
```

5. 跑分类 demo

```sh
/mnt/sd/tdl_build_sys/tdl_classify_demo \
  --image /mnt/sd/dog.jpg \
  --model-spec /mnt/git/jyd_tdl_app/configs/model_specs/cls_hand_gesture.ini \
  --firmware /mnt/git/jyd_tdl_app/firmware/libbm1688_kernel_module.so \
  --output /mnt/sd/dog_cls_syslibs.jpg
```

6. 跑相机抓图 demo

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

详细说明请看：

- [开发指南](DEV_GUIDE.md)
- [板端原生编译与测试指南](board_native_build_guide_zh.md)
