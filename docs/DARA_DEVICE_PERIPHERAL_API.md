# DARA 设备与外设接口说明

## 1. 总规则：先查 PinMap，再创建外设

应用不应猜测 Linux 总线号、GPIO line 或 PWM channel。先使用 `PinMap` 查看当前板子实际声明的标识，再将该标识传入构造函数：

```python
from dara.peripheral.pinmap import PinMap

print(PinMap.list_ids("gpio"))
print(PinMap.list_ids("i2c"))
print(PinMap.list_ids("pwm"))
print(PinMap.describe("i2c", "I2C2"))
```

`list_ids(kind)` 的 `kind` 可为：`adc`、`softkey`、`gpio`、`i2c`、`pwm`、`spi`、`uart`。

当前 CV184x 主板配置的结果为：

- GPIO：`LED`
- I2C：`I2C2`、`I2C4`
- ADC：`ADCBTN`
- UART：`UART1`、`UART2`
- PWM：当前为空

没有出现在 `PinMap.list_ids("pwm")` 中的 PWM 不能直接以数字方式猜测并使用。它需要同时确认原理图、DTS/PWM 驱动与 pinmux 后，才可加入 `cv184x_mainboard.py`。

## 2. GPIO

```python
from dara.peripheral.gpio import GPIO, GPIODirection, GPIOPull, GPIOEdge

with GPIO(
    "USER_LED",
    GPIODirection.OUT,
    initial=False,
    active_low=False,
) as led:
    led.high()
    led.low()
    led.toggle()
```

- `id`：PinMap 中的 GPIO 名称，如 `"USER_LED"`；不建议直接填 Linux line 数字。
- `direction`：`GPIODirection.IN` 或 `GPIODirection.OUT`。旧的 `HIGH`、`LOW` 仍兼容，但新代码推荐使用 `OUT + initial`。
- `initial`：仅输出模式可用；`True` 在申请 line 时设为高，`False` 设为低。
- `active_low`：逻辑反相。`write(True)` 对应物理低电平；是旧参数 `inverted` 的同义名称。
- `pull`：`GPIOPull.UP`、`DOWN`、`DISABLE`、`DEFAULT`。
- `edge`：输入事件使用 `GPIOEdge.RISING`、`FALLING`、`BOTH`。
- `poll(timeout)`：等待边沿；`timeout` 单位为秒，`None` 或负数无限等待，`0` 不等待。

注意：当前默认 `LED` 映射对应复用脚 `P3 / XGPIOA_26`。在没有确认它不是存储/其他关键功能复用脚前，示例只应以 `auto_open=False` 检查构造，不要在线上设备主动切换该脚。

## 3. PWM

```python
from dara.peripheral.pwm import PWM

# 前提："SERVO_PWM" 已由该板 PinMap 明确声明。
with PWM("SERVO_PWM", frequency=50, duty_cycle=0.075) as pwm:
    pwm.set_duty_percent(7.5)
    pwm.enable()
```

- `id`：PinMap 中的 PWM 名称。
- `frequency`：频率，单位 Hz。例如舵机通常 50 Hz。
- `duty_cycle`：占空比，范围 0.0～1.0。例如 7.5% 写为 `0.075`。
- `set_duty_percent(value)`：占空比百分数，范围 0.0～100.0；界面和教学场景推荐使用它。
- `freq`、`duty` 是旧参数名，继续兼容；不要和 `frequency`、`duty_cycle` 同时传入。
- `enable`：构造时是否立即使能；默认值来自 PinMap 的 PWM 配置。

## 4. I2C 与设备构造

所有 I2C 设备统一支持两种写法：

```python
from dara.peripheral.i2c import I2C
from dara.device.qmi8658 import QMI8658

# 推荐：总线归应用所有，多个设备可共享，设备关闭不会关闭 bus。
with I2C("I2C2") as bus:
    imu = QMI8658(bus)
    sample = imu.read_imu()
    imu.close()
    assert bus.is_opened

# 兼容旧代码：数字或字符串映射 ID 也可直接传入。
imu = QMI8658(2)
```

已统一支持上述行为的 I2C 设备：`QMI8658`、`LSM6DSOWTR`、`AHT20`、`AXP2101`、`MMC56X3`、`VL53L0X`、`LTR390`。

使用共享总线时，应用负责 `I2C` 的生命周期；设备 `close()` 只关闭传感器，不关闭调用方已经打开的总线。

## 5. QMI8658 与姿态解算

```python
from time import sleep
from dara.device.qmi8658 import QMI8658
from dara.peripheral.i2c import I2C

with I2C("I2C2") as bus:
    imu = QMI8658(bus)
    while True:
        attitude = imu.read_attitude()
        print(attitude.roll, attitude.pitch, attitude.yaw)
        sleep(0.02)
```

- `QMI8658(i2c, addr=0x6B, ...)`：当前板上实际检测到地址 `0x6B`。
- 默认配置调整为 `4g / 250Hz / 512dps / 250Hz`，适合常规姿态和交互；更高量程或频率可显式传 `IMUAccScale`、`IMUAccOdr`、`IMUGyroScale`、`IMUGyroOdr` 枚举。
- `read_imu()`：加速度单位为 g；陀螺仪默认单位为度/秒。
- `read_attitude(dt=None)`：QMI8658 内部自动创建并复用经典 98% 陀螺仪、2% 重力校正互补滤波器；`dt` 单位秒，实时模式可省略，离线回放可显式传入。
- `reset_attitude(roll=0, pitch=0, yaw=0)`：重置姿态和时间基准；适合“重置方向”操作或长时间暂停采样后恢复。
- `configure_attitude(correction=0.02, reference_hz=100)`：仅需调整滤波强度时调用。线性加速度大时降低 correction；静止时倾斜漂移大时增加 correction。
- `IMUAttitude.roll/pitch/yaw`：单位度；`roll_radians`、`pitch_radians`、`yaw_radians` 返回弧度。
- 仅六轴没有地磁绝对航向，`yaw` 是相对航向且会累积漂移；要获得罗盘航向，需要磁力计标定和九轴融合。

## 6. 资源释放

- GPIO/PWM/I2C 推荐使用 `with`。
- 设备对象可显式 `close()`；共享 I2C 时不会意外关闭应用持有的总线。
- 长时间程序退出前停止电机、舵机或 PWM 输出，再释放对象。
