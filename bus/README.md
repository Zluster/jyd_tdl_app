# Linux UART Python 工具

本目录提供 GD32 级联传感器通信、PA8 风扇控制、PAJ7620U2 手势识别、
ZW111 指纹控制（兼容原 ZW101 接口名）和 OTA 在线升级/恢复。仅依赖 Python 3.10+ 标准库。

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

扫描整条级联链路：

```python
from dara.bus.jydbus_bus import JydBus

with JydBus("/dev/ttyS2", 115200) as bus:
    sensors = bus.scan()
    for sensor_name, sensor_number in sensors:
        print(f"{sensor_name} number={sensor_number}")
```

`scan()` 返回所有已发现节点的 `(sensor_name, sensor_number)` 列表；没有收到响应时
返回空列表，串口或发送错误会抛出 `OSError`。需要完整命令状态时使用
`bus.run_command(JYDBUS_UART_COMMAND_SCAN)`。

WS2812B 示例：

```python
from devices import WS2812BPanel
from jydbus_bus import JydBus

colors = [(0, 0, 0)] * 128
colors[15] = (255, 0, 0)

with JydBus("/dev/ttyS2") as bus:
    panel = WS2812BPanel(bus, 1)
    panel.display_frame(colors)
```

设置 128 颗灯为同一种颜色时，传入 `(red, green, blue)` RGB 元组：

```python
panel.set_all_colors(color=(255, 0, 0))
```

整屏数据会自动分成两个 64 灯数据块，并在全部数据到达后统一刷新。需要同时更新多颗灯时，
应先修改 `colors` 数组，再调用一次 `display_frame()`。不要在循环中调用
`set_pixel_color()`：单灯命令每次都要重发整条 WS2812B 链，连续调用会产生
明显的前后灯珠刷新延迟。

PA8 风扇示例：

```python
from devices import FanActuator
from jydbus_bus import JydBus

with JydBus("/dev/ttyS2") as bus:
    fan = FanActuator(bus, 1)
    fan.set_speed(35)  # PA8 outputs 25 kHz PWM at 35% duty
    print(fan.read_state())
    fan.turn_off()
```

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
  --device /dev/ttyS2 --baud 115200 --version 2
```

Slot A/B 镜像链接地址不同，不能混用。`recovery` 仅用于 Linux UART 与
Bootloader 直连。版本号范围为 `1..0xFFFFFFFF`，必须单调递增；允许同版本
重刷，低于设备已确认版本的固件会被拒绝。可用 `status` 查看当前版本和已确认版本。

## 硬件工具

```bash
python3 scan_hardware.py --device /dev/ttyS2 --baud 115200
python3 examples/example_dual_uart.py --device /dev/ttyS2
python3 examples/paj7620_example.py --device /dev/ttyS2 --number 1
python3 zw101_cli.py /dev/ttyS2 match
python3 zw101_cli.py /dev/ttyS2 enroll  # 自动选择最小空闲 ID
python3 tests/mfrc522_debug.py --scan
```

同一时刻只能有一个程序占用同一个串口。
