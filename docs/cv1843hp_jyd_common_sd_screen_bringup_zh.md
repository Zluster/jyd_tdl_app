# CV1843HP JYD Common SD 屏幕、触摸与 MMF 显示接入说明

本文档针对 `cv1843hp_jyd_common_sd`，说明如何基于当前已经验证过的改动完成：

- U-Boot 阶段屏幕点亮与开机 `logo.jpeg` 显示
- Linux 阶段 GT911 触摸驱动接入
- 保持 `jyd_common_sd` 为 Linux-only 板型，不误迁 `dualos_alios` 的双系统分区
- 为后续 `tdl_app_sdk` 的 MMF/VO/OSD 显示测试提供稳定底座

## 1. 当前结论

`cv1843hp_jyd_common_sd_dualos_alios` 可以作为参考板型，但不能整板直接迁到 `cv1843hp_jyd_common_sd`。

原因：

- `dualos_alios` 带有双系统分区和 `yoc.bin`
- `dualos_alios` 的 defconfig 会打开 `CONFIG_DUAL_OS=y`
- 这些配置会改变 SD 分区布局和启动链路
- `jyd_common_sd` 当前目标是纯 Linux SD 镜像，不应该把这些一起带过去

可以直接参考和迁移的内容主要是：

- U-Boot 板级初始化里的个别 GPIO/寄存器置位
- 屏幕 panel 选择机制
- U-Boot 的开机 logo 加载链路
- 与屏幕点亮直接相关的 panel 配置

不应直接迁移的内容：

- `cv1843hp_jyd_common_sd_dualos_alios_defconfig`
- `partition_sd.xml` 里的 `2nd` 分区和 `yoc.bin`
- `CONFIG_DUAL_OS` / `CONFIG_ENABLE_ALIOS`

## 2. 当前建议保留的板级状态

### 2.1 U-Boot 阶段

保持：

- `panel=` 方式选择屏幕
- `logo.jpeg` 从 SD 启动分区加载
- `kd035qhfid161` 作为当前默认 panel

当前推荐默认：

```text
panel=kd035qhfid161
```

如果后续更换到 `dsi_hx8394_evb.h` 对应的屏幕，只需要：

1. 确认 U-Boot 里已有该 panel 的描述头文件和注册项
2. 修改 `/boot/board.env` 里的 `panel=...`
3. 必要时同步 lane / timing

### 2.2 Linux 阶段 GT911

当前远端已经验证过的状态是：

- I2C: `i2c0`
- 设备地址: `0x5d`
- `RST`: `GPIOA[19]`
- `INT`: `GPIOA[4]`

当前 DTS 为：

```dts
&i2c0 {
	status = "okay";
	clock-frequency = <400000>;

	gt911: touchscreen@5d {
		compatible = "goodix,gt9xx";
		reg = <0x5d>;
		goodix,rst-gpio = <&porta 19 GPIO_ACTIVE_HIGH>;
		goodix,irq-gpio = <&porta 4 GPIO_ACTIVE_LOW>;
		status = "okay";
	};
};
```

当前驱动策略为：

- `GTP_CONFIG_OF`
- `GTP_CUSTOM_CFG = 0`
- `GTP_DRIVER_SEND_CFG = 0`

这表示：

- GPIO 从 DTS 读取
- 分辨率和触摸参数优先使用 GT911 芯片内部配置
- DTS 里的 `goodix,cfg-group0` 目前不会真正下发给芯片

因此，如果你现在看到启动日志中：

```text
IC Version: 911_1060
X_MAX: 640, Y_MAX: 960
```

这代表当前生效的是 GT911 内部配置，不是驱动强推的配置。

## 3. 从 dualos_alios 需要迁什么

目前确认唯一值得从 `dualos_alios` 迁到 `jyd_common_sd` 的板级初始化差异是：

[`build/boards/cv184x/cv1843hp_jyd_common_sd/u-boot/cvi_board_init.c`](C:/Users/DELL/Documents/New%20project/_board_edit/build/boards/cv184x/cv1843hp_jyd_common_sd/u-boot/cvi_board_init.c)

应包含：

```c
mmio_setbits_32(0x030002d0, 1 << 9);
```

这处在 `dualos_alios` 中存在，而 Linux-only 的 `jyd_common_sd` 原先没有。它属于典型的可迁移最小板级初始化项，不会引入双系统副作用。

## 4. 不要迁的内容

下面这些不要从 `dualos_alios` 直接搬到 `jyd_common_sd`：

- `cv1843hp_jyd_common_sd_dualos_alios_defconfig`
- `build/boards/cv184x/cv1843hp_jyd_common_sd_dualos_alios/partition_sd.xml`
- `yoc.bin`
- `CONFIG_DUAL_OS=y`
- `CONFIG_ENABLE_ALIOS=y`

如果把这些迁过去，会带来：

- 启动参数变化
- SD 分区数量变化
- rootfs 所在分区变化
- 镜像打包行为变化

这会让当前 Linux-only 板型不稳定，甚至重新引出 `root=/dev/mmcblk0p2` / `mmcblk1p2` 一类问题。

## 5. 与 rootfs 启动相关的注意事项

SD 启动时，`ROOTFS` 设备应和实际卡槽设备一致。

你前面已经遇到过：

- 内核命令行等待 `/dev/mmcblk0p2`
- 实际枚举出来的是 `mmcblk1p2`

因此每次重新改分区或重建 `u-boot/include/cvipart.h` 后，建议执行：

```sh
cd /home/jyd/zwz/sophpi
python3 build/tools/common/image_tool/mkcvipart.py \
  build/boards/cv184x/cv1843hp_jyd_common_sd/partition_sd.xml \
  u-boot-2021.10/include
```

然后确认生成的 `ROOTFS_DEV` 正确。

## 6. /boot/ver 版本文件

当前 SD 镜像打包脚本已经支持在启动分区生成：

```text
/boot/ver
```

内容包括：

- `image`
- `git_short`
- `git_commit`
- `build_time`
- `board`
- `panel`

这个改动可以对所有 SD 镜像板型生效，不会破坏其他板级配置。

## 7. 编译方式

以下流程建议在 `cvitek-linux` 容器中执行。

### 7.1 进入 SDK

```sh
cd /home/jyd/zwz/sophpi
```

### 7.2 配置环境

```sh
source build/cvisetup.sh
defconfig cv1843hp_jyd_common_sd
```

如果你平时使用的是：

```sh
CHIP=cv1843hp BOARD=jyd_common_sd STORAGE_TYPE=sd cvi_setup_env
```

也可以继续保持原来的方式。

### 7.3 重新生成分区头

```sh
python3 build/tools/common/image_tool/mkcvipart.py \
  build/boards/cv184x/cv1843hp_jyd_common_sd/partition_sd.xml \
  u-boot-2021.10/include
```

### 7.4 编译 U-Boot / kernel / rootfs / 镜像

如果你想全量生成 SD 镜像，最稳妥的是完整构建：

```sh
build_all
pack_sd_image
```

如果你的环境没有这两个封装命令，就用项目原有编译脚本。

关键目标是最终得到：

- `install/soc_cv1843hp_jyd_common_sd/fip.bin`
- `install/soc_cv1843hp_jyd_common_sd/rawimages/boot.sd`
- `install/soc_cv1843hp_jyd_common_sd/rawimages/rootfs.sd`
- `install/soc_cv1843hp_jyd_common_sd/images/*.img`

### 7.5 如果只改了 U-Boot logo / panel

最少需要重新生成：

- `fip.bin`
- `boot.sd`
- 最终 `.img`

如果只替换 SD 卡 FAT 分区里的 `logo.jpeg`，通常不需要重新编译。

## 8. tdl_app_sdk 的编译方式

板级镜像稳定后，再构建 `tdl_app_sdk`。

```sh
cd /home/jyd/zwz/sophpi/tdl_app_sdk
export TOOLCHAIN_ROOT=/home/jyd/zwz/sophpi/host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf

cmake -S . -B build_verify/cv184x \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/home/jyd/zwz/sophpi/tdl_app_sdk/cmake/toolchains/arm-none-linux-musleabihf.cmake

cmake --build build_verify/cv184x --target tdl_mmf_osd_fill_demo -j
```

如果缓存已经污染，先清理：

```sh
rm -rf /home/jyd/zwz/sophpi/tdl_app_sdk/build_verify/cv184x
```

## 9. MMF 显示验证建议

不要优先用 `fb0` 方式做压力测试。

原因：

- `tdl_graphic_vo_demo` 会直接改 framebuffer layer 和 screeninfo
- 在当前板子上已经出现过整机无响应

更推荐的路径：

1. 先确认 Linux 正常起屏
2. 再确认媒体模块全部加载
3. 再用 MMF + RGN/VO 的 demo 做 OSD 显示

当前新增的 demo 是：

[`tdl_app_sdk/apps/tdl_mmf_osd_fill_demo.cpp`](C:/Users/DELL/Documents/New%20project/remote_tdl_app_sdk/apps/tdl_mmf_osd_fill_demo.cpp)

如果板端运行时报：

```text
RGN open fail
osd create failed: CVI_RGN_Create failed
```

这通常不是 demo 本身错误，而是板端运行环境里：

- `cv184x_rgn.ko` 没正确加载
- 对应设备节点没创建
- 或媒体系统初始化链路不完整

这时优先检查：

```sh
lsmod | grep -i rgn
ls -l /dev | grep -i rgn
dmesg | grep -i rgn
dmesg | grep -i -E 'vo|sys|mmf'
cat /system/ko/loadsystemko.sh
```

## 10. 屏幕更换方法

如果后续从 `kd035qhfid161` 换到其他 DSI 屏，例如 `hx8394`，推荐按下面做：

1. 在 U-Boot 里准备对应 panel 头文件和注册项
2. 保持公共镜像机制不变
3. 用 `/boot/board.env` 里的 `panel=` 选择不同屏幕

这样可以做到：

- 一个公共镜像
- 通过 `panel=kd035qhfid161` / `panel=hx8394...` 切换
- 避免每块屏都维护一套独立板型

## 11. 当前推荐验证顺序

1. 先确认 U-Boot logo 正常显示
2. 进入 Linux 后确认触摸枚举正常
3. 用 `cat /boot/ver` 确认镜像版本、board 和 panel
4. 检查 `/system/ko/loadsystemko.sh` 是否已加载媒体模块
5. 再测试 `tdl_app_sdk` 的 MMF OSD demo

## 12. 当前相关文件

板级 DTS：

- `linux_5.10/arch/arm/boot/dts/cvitek/cv1843hp_jyd_common_sd.dts`
- `build/boards/cv184x/cv1843hp_jyd_common_sd/dts_arm/cv1843hp_jyd_common_sd.dts`

GT911：

- `osdrv/extdrv/tp/ts_gt9xx/gt9xx.h`
- `osdrv/extdrv/tp/ts_gt9xx/gt9xx.c`

U-Boot：

- `build/boards/cv184x/cv1843hp_jyd_common_sd/u-boot/cvi_board_init.c`
- `u-boot-2021.10/include/configs/cv184x-asic.h`
- `u-boot-2021.10/include/cvitek/cvi_panels/cvi_panels.h`
- `u-boot-2021.10/include/cvitek/cvi_panels/cvi_panel_diffs.h`
- `u-boot-2021.10/cmd/cvi_vo.c`

SD 打包：

- `build/tools/common/sd_tools/board.env`
- `build/tools/common/sd_tools/genimage_rootless.cfg`
- `build/tools/common/sd_tools/sd_gen_burn_image_rootless.sh`
