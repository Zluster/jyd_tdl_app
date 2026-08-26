# Linux UART Python API 使用说明

适用环境：Linux、Python 3.10.14、115200 8N1。仅使用标准库，不需要安装第三方包。

> 同一时刻只能有一个进程打开同一个串口。推荐使用 `with`，退出时会自动关闭串口和接收线程。

## 1. 传感器 API

优先使用 `jydbusApi`：

```python
import time

from jydbus_api import jydbusApi
from jydbus_uart import JYDBUS_TYPE_BMP390

with jydbusApi("/dev/ttyS2", 115200) as api:
    api.query(JYDBUS_TYPE_BMP390, 1)  # 只发送查询帧
    time.sleep(0.35)                  # 等待设备响应
    data = api.read(JYDBUS_TYPE_BMP390, 1)
    print(data.value)
```

主要方法：

| 方法 | 作用 | 返回值 |
|---|---|---|
| `query(type, number)` | 发送一次查询，不等待响应 | 成功返回 `0` |
| `read(type, number)` | 读取接收线程缓存的最新数据 | `JydbusData` |
| `write(type, number, value)` | 写入 32 位设置值 | 成功返回 `0` |
| `command(cmd, type=0, number=0, value=0)` | 执行统一命令 1～8 | `JydbusUartCommandResult` |
| `close()` | 关闭串口 | 无 |

需要“发送并等待响应”时，使用命令 8：

```python
from jydbus_api import jydbusApi
from jydbus_uart import JYDBUS_UART_COMMAND_QUERY_SENSOR, JYDBUS_TYPE_AHT10

with jydbusApi("/dev/ttyS2") as api:
    result = api.command(JYDBUS_UART_COMMAND_QUERY_SENSOR,
                         JYDBUS_TYPE_AHT10, 1)
    if result.status == 0 and result.data_valid:
        print(result.data.value)
```

命令编号：

| 命令 | 常量 | 功能 |
|---:|---|---|
| 1 | `JYDBUS_UART_COMMAND_SCAN` | 扫描传感器 |
| 2 | `JYDBUS_UART_COMMAND_QUERY_ALL` | 查询全部传感器 |
| 3 | `JYDBUS_UART_COMMAND_ENABLE_AUTO_UPLOAD` | 开启自动上传 |
| 4 | `JYDBUS_UART_COMMAND_DISABLE_AUTO_UPLOAD` | 关闭自动上传 |
| 5 | `JYDBUS_UART_COMMAND_SET_WS2812B_RED` | 第一个 WS2812B 设为红色 |
| 6 | `JYDBUS_UART_COMMAND_READ_SENSOR` | 读取缓存 |
| 7 | `JYDBUS_UART_COMMAND_WRITE_SENSOR` | 写入数值 |
| 8 | `JYDBUS_UART_COMMAND_QUERY_SENSOR` | 查询并等待响应 |

### 扫描帧协议

扫描使用统一命令 `JYDBUS_UART_COMMAND_SCAN = 1`，对应帧类型
`JYDBUS_FRAME_TYPE_SCAN = 0x03`。主机发送一次扫描请求，在线节点分别返回
一帧扫描应答。

请求帧（主机 → 总线）：

```text
55 | LEN | 03 | 00 | 00 | 3C 3C 3C 3C | CRC16_L CRC16_H | AA
```

响应帧（节点 → 主机）：

```text
55 | LEN | 03 | SENSOR_TYPE | SENSOR_NUMBER | 4B 4B 4B 4B | CRC16_L CRC16_H | AA
```

| 字段 | 长度 | 含义 |
|---|---:|---|
| `0x55` | 1 字节 | 帧头 |
| `LEN` | 1 字节 | payload 长度；不包含帧头、CRC 和帧尾，扫描帧为 `4` |
| `0x03` | 1 字节 | 扫描帧类型 |
| `SENSOR_TYPE` | 1 字节 | 节点传感器类型，例如 `0x03` 表示 BMP390 |
| `SENSOR_NUMBER` | 1 字节 | 节点编号，通常为 `1～8` |
| payload | 4 字节 | 请求固定为 ASCII `<<<<`；响应前 4 字节必须为 ASCII `KKKK` |
| `CRC16_L CRC16_H` | 2 字节 | CRC16-Modbus，低字节在前 |
| `0xAA` | 1 字节 | 帧尾 |

CRC16-Modbus 的计算范围从 `LEN` 开始，一直到 payload 最后一个字节，
不包括 `0x55`、CRC 本身和 `0xAA`。收到的帧必须同时满足帧头、帧尾、长度、
帧类型、`KKKK` 标记和 CRC 校验，才会加入扫描结果。

最简单的 Python 调用方式：

```python
from examples.api_call_example import getScanData, initialize

api = initialize("/dev/ttyS2", 115200)
try:
    for item in getScanData(api):
        print(item)
finally:
    api.close()
```

`getScanData()` 返回的每项包含 `sensor_type`、`jydbus_name`、
`sensor_number`、`response_received`、`response_ms` 和 `send_result`。
`status == 0` 且列表非空表示扫描成功；空列表时检查设备是否运行应用程序、
TX/RX/GND 是否交叉连接以及波特率是否为 `115200`。统计信息中的
`crc_errors` 表示 CRC 错误，`format_errors` 表示帧头、长度或帧尾等格式错误。

传感器类型：

| 类型 | 常量 | `data.value` 主要字段 |
|---:|---|---|
| `0x02` | `JYDBUS_TYPE_AHT10` | `temperature_c`, `humidity_percent` |
| `0x03` | `JYDBUS_TYPE_BMP390` | `temperature_c`, `pressure_pa` |
| `0x04` | `JYDBUS_TYPE_MAX30102` | `heart_rate_bpm`, `spo2_percent` |
| `0x05` | `JYDBUS_TYPE_VL53L0X` | `distance_mm` |
| `0x06` | `JYDBUS_TYPE_MFRC522` | `uid`, `tag_type`, `present` |
| `0x07` | `JYDBUS_TYPE_WS2812B` | `ws2812b_ack` |
| `0x08` | `JYDBUS_TYPE_ZW101` | `operation`, `status`, `fingerprint_id`, `score` |
| `0x09` | `JYDBUS_TYPE_BUTTON_PB1` | `button_level` |
| `0x0A` | `JYDBUS_TYPE_JOYSTICK` | `x_adc`, `y_adc` |
| `0x0F` | `JYDBUS_TYPE_PHOTORESISTOR_ADC` | `adc` |
| `0x11` | `JYDBUS_TYPE_WATER_LEVEL_ADC` | `water_level_adc` |
| `0x12` | `JYDBUS_TYPE_SOIL_MOISTURE_ADC` | `soil_moisture_adc` |
| `0x13` | `JYDBUS_TYPE_ZSPD4003` | `heart_rate_bpm`, `spo2_percent`, `status`, `signal_quality` |
| `0x14` | `JYDBUS_TYPE_KNOB_SWITCH_ADC` | `knob_switch_adc` |

### WS2812B 灯板控制

WS2812B 类型为 `0x07`，灯珠索引为 `0～127`，颜色使用 `0x00RRGGBB`。
单灯控制和 128 灯整屏控制示例：

```python
from jydbus_api import jydbusApi

colors = [0x000000] * 128
colors[0] = 0xFF0000       # 第 0 颗红色
colors[127] = 0x0000FF     # 第 127 颗蓝色

with jydbusApi("/dev/ttyS2", 115200) as api:
    api.set_ws2812b_pixel(12, 0x00FF00)  # 第 12 颗绿色
    api.set_ws2812b_frame(colors)        # 一次提交整屏 128 颗颜色
```

`set_ws2812b_frame()` 会在内部将 128 颗颜色分成 16 个通信块传输，
所有块校验成功后才提交到灯板；任意块失败会抛出异常，灯板保持原显示。

`JydbusData` 常用字段：

```text
sensor_type, sensor_number     传感器类型和编号
raw                            原始 payload（bytes）
decoded_valid                  是否成功解码
value                          解码后的 dict
sequence                       每次收到新数据递增
updated_monotonic_ms           本机单调时钟时间戳
```

底层控制可直接使用 `JydbusUart`：

```python
import time

from jydbus_uart import JydbusUart

with JydbusUart("/dev/ttyS2", 115200) as uart:
    uart.query(0x03, 1)
    time.sleep(0.35)
    print(uart.read_all())
    print(uart.get_stats())
```

函数式入口为 `jydbus_api_open()` / `jydbus_api_close()`；新代码推荐使用
`with jydbusApi(...)`。为匹配现有集成，也提供完全相同的 `jydbusApi` 类别名。
旧 `SensorApi` 和 `sensor_api_*` 名称仍可兼容使用。

### 每种 Jydbus 设备对应的类

专用类定义在 `jydbus_devices.py`。一个 `jydbusApi` 代表一条 UART 总线；所有传感器
对象必须共享它，不能为每个传感器重复打开 `/dev/ttyS2`：

```python
from jydbus_api import jydbusApi
from jydbus_devices import JydbusAHT10, JydbusBMP390

with jydbusApi("/dev/ttyS2", 115200) as api:
    aht10 = JydbusAHT10(api, sensor_number=1)
    bmp390 = JydbusBMP390(api, sensor_number=2)

    print(aht10.query_value())
    print(bmp390.query_value())
```

类与类型对应关系：

| 类型 | 类 |
|---:|---|
| `0x02` | `JydbusAHT10` |
| `0x03` | `JydbusBMP390` |
| `0x04` | `JydbusMAX30102` |
| `0x05` | `JydbusVL53L0X` |
| `0x06` | `JydbusMFRC522` |
| `0x07` | `JydbusWS2812B` |
| `0x08` | `JydbusZW101` |
| `0x09` | `JydbusButtonPB1` |
| `0x0A` | `JydbusJoystick` |
| `0x0F` | `JydbusPhotoresistorADC` |
| `0x11` | `JydbusWaterLevelADC` |
| `0x12` | `JydbusSoilMoistureADC` |
| `0x13` | `JydbusZSPD4003` |
| `0x14` | `JydbusKnobSwitchADC` |
| `0x15` | `JydbusPAJ7620U2` |

所有类继承 `JydbusDevice`，提供以下通用方法：

| 方法 | 作用 |
|---|---|
| `query()` | 只发送查询，不等待响应 |
| `query_data()` | 查询并等待响应，返回 `JydbusData` |
| `query_value()` | 查询并返回解码后的字典 |
| `read()` | 读取接收线程缓存，返回 `JydbusData` |
| `read_value()` | 读取缓存并返回解码后的字典 |
| `write(value)` | 写入 32 位配置值 |
| `set_auto_upload(enabled, interval_ms=None)` | 开启或关闭该节点自动上传 |

也可以让 `jydbusApi` 根据类型自动选择类：

```python
with jydbusApi("/dev/ttyS2") as api:
    bmp390 = api.jydbus(0x03, 1)
    print(type(bmp390).__name__)  # JydbusBMP390
```

WS2812B 专用方法：

```python
from jydbus_devices import JydbusWS2812B

with jydbusApi("/dev/ttyS2") as api:
    leds = JydbusWS2812B(api, 1)
    leds.set_pixel(0, 0xFF0000)
    leds.set_frame([0x000000] * 128)
```

`JydbusZW101` 提供 `set_timeout()`、`enroll()`、`match()`、`delete()` 和
`clear()`；这些方法复用 `jydbusApi` 的 UART，不会再次打开串口。原有
`Zw101Device` 接口继续保留，兼容旧代码。

## 2. ZW101 API

```python
from zw101_api import Zw101Device

with Zw101Device("/dev/ttyS2", node_number=1) as fingerprint:
    fingerprint.set_timeout(35000)

    fingerprint.enroll(2)       # 同一手指按下并完全松开三次
    result = fingerprint.match()
    print(result.fingerprint_id, result.score)

    fingerprint.delete(2)
    # fingerprint.clear()        # 清空全部指纹，谨慎调用
```

主要方法：

| 方法 | 作用 |
|---|---|
| `enroll(id)` | 录入 ID，范围 `0～49` |
| `match()` | 1:N 匹配 |
| `delete(id)` | 删除指定 ID |
| `clear()` | 清空指纹库 |
| `set_timeout(ms)` | 设置操作超时，默认 35000 ms |

返回对象常用字段：`status`、`module_status`、`fingerprint_id`、`score`、`response_ms`。

模块同时提供 `zw101_open()`、`zw101_enroll()`、`zw101_match()`、`zw101_delete()`、`zw101_clear()` 等函数式入口；新代码推荐直接使用 `Zw101Device`。

## 3. OTA API

正常级联升级：

```python
from sensor_ota import SensorOta


def progress(sent: int, total: int) -> None:
    print(f"{sent}/{total}")


with SensorOta("/dev/ttyS2", 115200, debug=False) as ota:
    status = ota.get_status(sensor_type=0x09, sensor_number=1)
    print(status.active_slot, status.confirmed_slot)

    ota.upgrade_ab(
        sensor_type=0x09,
        sensor_number=1,
        slot_a_path="Project_OTA_Slot_A.bin",
        slot_b_path="Project_OTA_Slot_B.bin",
        version=2,
        progress=progress,
    )
```

指定目标 Slot：

```python
from sensor_ota import SENSOR_OTA_SLOT_B, SensorOta

with SensorOta("/dev/ttyS2") as ota:
    ota.upgrade_file(0x09, 1, SENSOR_OTA_SLOT_B,
                     "Project_OTA_Slot_B.bin", version=2)
```

Bootloader 直连恢复：

```python
from sensor_ota import SENSOR_OTA_SLOT_A, SensorOta

with SensorOta("/dev/ttyS2", debug=True) as ota:
    ota.recovery_upgrade_file(
        SENSOR_OTA_SLOT_A,
        "Project_OTA_Slot_A.bin",
    )
```

主要方法：

| 方法 | 作用 |
|---|---|
| `get_status(type, number)` | 查询 A/B Slot 状态 |
| `upgrade_ab(...)` | 查询当前 Slot，自动升级另一侧 |
| `upgrade_file(...)` | 升级指定 Slot |
| `recovery_upgrade_file(slot, image, progress=None)` | Bootloader UART 直连恢复 |

`SensorOtaStatus` 常用字段：`active_slot`、`confirmed_slot`、`pending_slot`、`download_slot`、`next_offset`、`image_size`、`image_version`。

模块还提供 `sensor_ota_open()` / `sensor_ota_close()`；新代码推荐使用 `with SensorOta(...)`。

## 4. 异常处理

接口失败会抛出 `OSError`、`TimeoutError` 或 `OtaDeviceError`：

```python
import errno

try:
    # 调用 API
    pass
except TimeoutError:
    print("设备响应超时")
except OSError as exc:
    print(f"操作失败: errno={exc.errno}, message={exc}")
```

常见错误：

| 错误 | 含义 |
|---|---|
| `ENODATA` | 缓存中还没有该传感器数据 |
| `EINVAL` | 参数、编号或镜像不合法 |
| `ETIMEDOUT` | 设备未在规定时间内响应 |
| `EBUSY` | ZW101 正忙 |

## 5. 使用约束

- 传感器编号有效范围为 `1～8`；ZW101 指纹 ID 为 `0～49`。
- `read()` 不会发送查询，实时读取请先 `query()`，或使用命令 8。
- OTA、传感器监听和 ZW101 工具不能同时占用同一个串口。
- `recovery_upgrade_file()` 仅用于 Linux UART 与 Bootloader 直连。
- Slot A/B 镜像链接地址不同，不能混用。

## 6. API 测试

测试条件：

- Linux，Python 3.10.14；默认串口 `/dev/ttyS2`，115200 8N1。
- Linux UART TX 接板端 RX，Linux UART RX 接板端 TX，并连接 GND。
- 普通 API 和 `ota-status` 测试时，板端应运行应用程序；只有 Recovery 才进入 Bootloader。
- 同一时刻只能有一个程序使用 `/dev/ttyS2`，不要同时运行 `cat /dev/ttyS2`、串口工具或其他 OTA 程序。

先检查串口是否被占用：

```bash
lsof 2>/dev/null | grep '/dev/ttyS2'
```

无输出表示当前没有进程占用。测试代码统一位于 `tests/`。进入代码目录后执行：

```bash
cd linux_uart_Python

# 无需板卡和串口：验证协议、CRC、数据解析、WS2812B 分块和 OTA 镜像
python3 -m unittest discover -s tests -p "test_*.py" -v

# 硬件 API 测试：默认 /dev/ttyS2
python3 tests/api_simple_test.py
```

运行硬件测试前，根据实际设备修改 `tests/api_simple_test.py` 顶部的
`DEVICE`、`SENSOR_TYPE` 和 `SENSOR_NUMBER`。该测试会临时开启自动上传、查询和读取
数据，并将 1 号 WS2812B 设置为红色，因此会改变设备状态。详细说明见
`tests/README.md`。
