# Linux UART Python 工具

本目录提供 GD32 级联传感器通信、PAJ7620U2 手势识别、ZW101 指纹控制和
OTA 在线升级/恢复。仅依赖 Python 3.10+ 标准库。

## 快速自检

```bash
cd linux_uart_Python
python3 ota_cli.py selftest
python3 -m unittest discover -s tests -p "test_*.py" -v
```

上述检查不访问硬件。串口默认为 `/dev/ttyS2`、`115200 8N1`；TX/RX 交叉并共地。

## 按传感器类型使用

一条总线只创建一个 `JydBus`，所有设备对象共享它：

```python
from devices import AHT10Sensor, BMP390Sensor
from jydbus_bus import JydBus

with JydBus("/dev/ttyS2", 115200) as bus:
    climate = AHT10Sensor(bus, 1)
    pressure = BMP390Sensor(bus, 2)
    print(climate.measure_temperature_humidity())
    print(pressure.measure_temperature_pressure())
```

主动查询使用具体传感器方法；读取已接收缓存使用 `read_cached_value()`。
通用操作为 `request_update()`、`request_data()`、`write_value()`、
`enable_auto_upload()` 和 `disable_auto_upload()`。

WS2812B 示例：

```python
from devices import WS2812BPanel
from jydbus_bus import JydBus

colors = [0x000000] * 128
colors[15] = 0xFF0000

with JydBus("/dev/ttyS2") as bus:
    panel = WS2812BPanel(bus, 1)
    panel.set_pixel_color(0, 0x0000FF)
    panel.display_frame(colors)
```

整屏数据会自动分块，并在全部通信块校验成功后统一刷新。

完整类名、方法和协议说明见 [使用说明.md](使用说明.md)。

## OTA 在线升级

OTA 功能保留在 `ota_cli.py` 和 `sensor_ota.py`，支持 A/B 槽升级、断点续传和
Bootloader 直连恢复。

```bash
python3 ota_cli.py status --type 0x15 --number 1 --device /dev/ttyS2 --baud 115200

python3 ota_cli.py upgrade --type 0x15 --number 1 \
  --device /dev/ttyS2 --slot-a Project_OTA_Slot_A.bin \
  --slot-b Project_OTA_Slot_B.bin --version 2

python3 ota_cli.py recovery --slot A --image Project_OTA_Slot_A.bin \
  --device /dev/ttyS2 --baud 115200
```

Slot A/B 镜像链接地址不同，不能混用。`recovery` 仅用于 Linux UART 与
Bootloader 直连。

## 硬件工具

```bash
python3 examples/example_dual_uart.py --device /dev/ttyS2
python3 examples/paj7620_example.py --device /dev/ttyS2 --number 1
python3 zw101_cli.py /dev/ttyS2 match
python3 tests/mfrc522_debug.py --scan
```

同一时刻只能有一个程序占用同一个串口。
