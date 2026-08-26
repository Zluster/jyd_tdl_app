# Linux UART Python 工具

本目录提供 GD32 级联传感器的 Linux Python 3 工具，包含通用传感器通信、PAJ7620U2 手势识别、ZW101 指纹模块和 **OTA 在线升级/恢复**。仅依赖 Python 标准库，不需要安装第三方包。

## 环境与接线

- Linux、Python 3.10 或更高版本。
- 默认串口：`/dev/ttyS2`，参数：`115200 8N1`。
- Linux TX 接 GD32 RX，Linux RX 接 GD32 TX，并且双方 GND 共地。
- 同一时刻只能由一个程序占用同一个串口。

确认串口设备和权限：

```bash
ls -l /dev/ttyS2
sudo usermod -aG dialout "$USER"  # 无串口权限时执行，之后重新登录
```

## 快速自检

```bash
cd linux_uart_Python
python3 ota_cli.py selftest
python3 -m unittest discover -s tests -p "test_*.py" -v
```

预期看到 `OTA protocol selftest: PASS`。该自检不访问硬件。

测试代码统一位于 `tests/`，硬件 API 测试可运行：

```bash
python3 tests/api_simple_test.py
```

详细条件见 `tests/README.md`。

扫描和 MFRC522 等专项硬件调试脚本也统一位于 `tests/`。

## 通用传感器通信

交互式工具：

```bash
python3 examples/example_dual_uart.py --device /dev/ttyS2 --baud 115200
```

常用命令：

```text
1             # 扫描传感器
8 0x15 1      # 查询 PAJ7620U2 一次
6 0x15 1      # 读取缓存中的最新 PAJ7620U2 数据
3             # 开启自动上传；PAJ7620U2 为 100 ms，其他为 1000 ms
4             # 关闭自动上传
```

基础 API：

```python
from jydbus_api import jydbusApi

with jydbusApi("/dev/ttyS2", 115200) as api:
    api.query(0x03, 1)
    print(api.read(0x03, 1).value)
```

`read()` 只读取接收线程缓存，不会隐式发送查询；首次读取前应先查询或开启自动上传。

推荐使用 Jydbus 专用类。所有对象共享同一个 `jydbusApi`，因此只打开一次串口：

```python
from jydbus_api import jydbusApi
from jydbus_devices import JydbusAHT10, JydbusBMP390

with jydbusApi("/dev/ttyS2", 115200) as api:
    aht10 = JydbusAHT10(api, 1)
    bmp390 = JydbusBMP390(api, 2)
    print(aht10.query_value())
    print(bmp390.query_value())
```

也可以按协议类型创建对应类：

```python
device = api.jydbus(0x03, 1)  # 返回 JydbusBMP390
```

全部类名和方法见 `API_使用说明.md`，可运行示例见
`examples/jydbus_classes_example.py`。

WS2812B 灯板（类型 `0x07`）支持单灯和整屏控制。灯珠编号为 `0～127`，颜色为
`0x00RRGGBB`：

```python
from jydbus_api import jydbusApi

colors = [0x000000] * 128
colors[15] = 0xFF0000

with jydbusApi("/dev/ttyS2", 115200) as api:
    api.set_ws2812b_pixel(15, 0x0000FF)
    api.set_ws2812b_frame(colors)
```

整屏 API 会自动分块传输并在固件端完整接收后统一刷新，避免传输过程中出现半屏状态。

## PAJ7620U2 手势识别

PAJ7620U2 由 GD32 的 `PB6/SCL`、`PB7/SDA` 驱动，Linux 通过 UART 获取结果：

```bash
python3 examples/paj7620_example.py --device /dev/ttyS2 --baud 115200 --number 1
```

程序会为传感器类型 `0x15` 开启手势事件上报；GD32 每识别一个新手势立即上传一次，不会周期性发送空状态或重复帧，连续相同手势也会分别打印。按 `Ctrl+C` 后会自动关闭。详细说明见 `../linux_uart/PAJ7620U2_使用说明.md`。

## OTA 在线升级

OTA 功能必须保留，相关代码为 `ota_cli.py` 和 `sensor_ota.py`。升级采用 A/B 槽：工具只写入当前未运行槽；升级成功后，应用稳定运行约 5 秒会确认新槽。若新槽连续启动失败，Bootloader 会回退到已确认槽。

### 查询 OTA 状态

正常应用运行且通过级联 UART 连接时，先查询状态：

```bash
python3 ota_cli.py status \
  --type 0x09 --number 1 \
  --device /dev/ttyS2 --baud 115200 --debug
```
改成1行：
python3 ota_cli.py status --type 0x15 --number 1 --device /dev/ttyS2 --baud 115200 --debug


将 `--type` 和 `--number` 改为目标传感器的实际地址。正常示例：

```text
active_slot=A confirmed_slot=A pending_slot=none
```

### 自动选择未运行槽升级

准备与目标传感器、槽位对应的两个 BIN 文件。Slot A 与 Slot B 链接地址不同，**不能互换，也不能使用错误传感器类型的镜像**。

```bash
python3 ota_cli.py upgrade \
  --type 0x09 --number 1 \
  --device /dev/ttyS2 --baud 115200 \
  --slot-a Project_OTA_Slot_A.bin \
  --slot-b Project_OTA_Slot_B.bin \
  --version 2 --debug
```
改成1行：
python3 ota_cli.py upgrade --type 0x15 --number 1 --device /dev/ttyS2 --baud 115200 --slot-a Project_OTA_Slot_A.bin --slot-b Project_OTA_Slot_B.bin --version 2 --debug

工具会先查询当前槽，再自动选取另一槽及对应镜像。传输完成后等待设备复位并稳定运行 5 秒，再执行一次 `status`：应满足 `active_slot` 与 `confirmed_slot` 相同，且 `pending_slot=none`。

仅升级指定槽时使用：

```bash
python3 ota_cli.py upgrade-one \
  --type 0x09 --number 1 \
  --slot B --image Project_OTA_Slot_B.bin \
  --version 2 --device /dev/ttyS2 --debug
```

### 断电/中断恢复验证

使用 `--stop-after` 可在指定字节数后故意中断传输；设备复位后应仍运行旧的已确认固件：

```bash
python3 ota_cli.py upgrade \
  --type 0x09 --number 1 \
  --slot-a Project_OTA_Slot_A.bin \
  --slot-b Project_OTA_Slot_B.bin \
  --version 2 --stop-after 4096 --debug
```

### Bootloader 直连恢复

仅在两个应用槽都无法启动、设备停在 Bootloader 时使用。Linux UART 必须**直接连接目标 GD32**，不能经过级联传感器链路：

```bash
python3 ota_cli.py recovery \
  --slot A --image Project_OTA_Slot_A.bin \
  --device /dev/ttyS2 --baud 115200 --debug
```

`recovery` 不会让正常运行的应用自动进入 Bootloader。

## ZW101 指纹模块

```bash
python3 zw101_cli.py /dev/ttyS2 enroll 2
python3 zw101_cli.py /dev/ttyS2 match
python3 zw101_cli.py /dev/ttyS2 delete 2
python3 zw101_cli.py /dev/ttyS2 clear
```

## 常见问题

- `Permission denied`：配置 `dialout` 权限并重新登录。
- `Device or resource busy`：关闭占用串口的其他工具。
- OTA `Operation timed out`：确认目标已烧录支持 OTA 的固件、类型/编号正确、级联接线正确。
- `Image vector does not match slot`：Slot A/B 镜像使用错误；重新确认镜像对应的链接地址。
- PAJ7620U2 无数据：确认 `SENSOR_15_EN` 已启用，PB6/PB7 与模块供电正确。

完整 OTA 设计与首次烧录流程见 `../linux_uart/OTA_流程与操作指南.md`。
