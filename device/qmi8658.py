"""QMI8658 six-axis inertial measurement unit driver."""


from math import isfinite, sqrt
from struct import unpack
from time import monotonic, sleep

from dara.core._error_helpers import wrap_error_as
from dara.device.imu import (
    IMU,
    IMUAccOdr,
    IMUAccScale,
    IMUData,
    IMUError,
    IMUGyroOdr,
    IMUGyroScale,
    IMUMode,
    imu_drivers,
)
from dara.device.ts import TS, TSData, TSError, ts_drivers
from dara.peripheral.i2c import I2C


class QMI8658Error(TSError, IMUError):
    """Raised when a QMI8658 operation cannot be completed."""


class QMI8658Data(TSData, IMUData):
    """Complete accelerometer, gyroscope, and temperature sample."""

    def __init__(self, acc, gyro, temperature):
        """Initialize a complete QMI8658 sample."""
        self.acc = acc
        self.gyro = gyro
        self.temperature = temperature


@imu_drivers.register("qmi8658")
@ts_drivers.register("qmi8658")
class QMI8658(IMU, TS):
    """A QMI8658 accelerometer and gyroscope connected over I2C."""

    _WHO_AM_I = 0x00
    _CTRL1 = 0x02
    _CTRL2 = 0x03
    _CTRL3 = 0x04
    _CTRL7 = 0x08
    _CTRL9 = 0x0A
    _CAL1_L = 0x0B
    _STATUSINT = 0x2D
    _DATA = 0x33
    _COD_STATUS = 0x46
    _DVX_L = 0x51
    _RESET = 0x60
    _DEVICE_ID = 0x05

    _STATUSINT_CMD_DONE = 0x80
    _CTRL_CMD_ACK = 0x00
    _CTRL_CMD_ACCEL_HOST_DELTA_OFFSET = 0x09
    _CTRL_CMD_GYRO_HOST_DELTA_OFFSET = 0x0A
    _CTRL_CMD_ON_DEMAND_CALIBRATION = 0xA2

    _COD_FAILURES = (
        (0x80, "gyro X sensitivity is below the COD low limit"),
        (0x40, "gyro X sensitivity is above the COD high limit"),
        (0x20, "gyro Y sensitivity is below the COD low limit"),
        (0x10, "gyro Y sensitivity is above the COD high limit"),
        (0x08, "significant vibration was detected during COD"),
        (0x04, "gyroscope startup failed during COD"),
        (0x02, "gyroscope was enabled when COD was started"),
        (0x01, "COD did not produce new gain parameters"),
    )

    _ACC_ODR = {
        IMUAccOdr.ACC_ODR_8000: 0,
        IMUAccOdr.ACC_ODR_4000: 1,
        IMUAccOdr.ACC_ODR_2000: 2,
        IMUAccOdr.ACC_ODR_1000: 3,
        IMUAccOdr.ACC_ODR_500: 4,
        IMUAccOdr.ACC_ODR_250: 5,
        IMUAccOdr.ACC_ODR_125: 6,
        IMUAccOdr.ACC_ODR_62_5: 7,
        IMUAccOdr.ACC_ODR_31_25: 8,
        IMUAccOdr.ACC_ODR_128: 12,
        IMUAccOdr.ACC_ODR_21: 13,
        IMUAccOdr.ACC_ODR_11: 14,
        IMUAccOdr.ACC_ODR_3: 15,
    }
    _ACC_SCALE = {
        IMUAccScale.ACC_SCALE_2G: 0,
        IMUAccScale.ACC_SCALE_4G: 1,
        IMUAccScale.ACC_SCALE_8G: 2,
        IMUAccScale.ACC_SCALE_16G: 3,
    }
    _GYRO_SCALE = {
        IMUGyroScale.GYRO_SCALE_16DPS: 0,
        IMUGyroScale.GYRO_SCALE_32DPS: 1,
        IMUGyroScale.GYRO_SCALE_64DPS: 2,
        IMUGyroScale.GYRO_SCALE_128DPS: 3,
        IMUGyroScale.GYRO_SCALE_256DPS: 4,
        IMUGyroScale.GYRO_SCALE_512DPS: 5,
        IMUGyroScale.GYRO_SCALE_1024DPS: 6,
        IMUGyroScale.GYRO_SCALE_2048DPS: 7,
    }
    _GYRO_ODR = {
        IMUGyroOdr.GYRO_ODR_8000: 0,
        IMUGyroOdr.GYRO_ODR_4000: 1,
        IMUGyroOdr.GYRO_ODR_2000: 2,
        IMUGyroOdr.GYRO_ODR_1000: 3,
        IMUGyroOdr.GYRO_ODR_500: 4,
        IMUGyroOdr.GYRO_ODR_250: 5,
        IMUGyroOdr.GYRO_ODR_125: 6,
        IMUGyroOdr.GYRO_ODR_62_5: 7,
        IMUGyroOdr.GYRO_ODR_31_25: 8,
    }

    def __init__(
        self,
        i2c,
        addr = 0x6B,
        mode = IMUMode.DUAL,
        acc_scale = IMUAccScale.ACC_SCALE_2G,
        acc_odr = IMUAccOdr.ACC_ODR_8000,
        gyro_scale = IMUGyroScale.GYRO_SCALE_16DPS,
        gyro_odr = IMUGyroOdr.GYRO_ODR_8000,
        *,
        auto_open = True,
    ):
        """Create a QMI8658 on a caller-owned bus and optionally initialize it."""
        if not isinstance(i2c, I2C):
            raise ValueError("i2c must be an I2C instance")
        if not isinstance(addr, int) or isinstance(addr, bool) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        for name, value, enum_type in (
            ("mode", mode, IMUMode),
            ("acc_scale", acc_scale, IMUAccScale),
            ("acc_odr", acc_odr, IMUAccOdr),
            ("gyro_scale", gyro_scale, IMUGyroScale),
            ("gyro_odr", gyro_odr, IMUGyroOdr),
        ):
            if not isinstance(value, enum_type):
                raise ValueError(f"{name} must be an IMU.{enum_type.__name__} value")
        for name, value, supported in (
            ("acc_odr", acc_odr, self._ACC_ODR),
            ("gyro_scale", gyro_scale, self._GYRO_SCALE),
            ("gyro_odr", gyro_odr, self._GYRO_ODR),
        ):
            if value not in supported:
                raise ValueError(f"{name} is not supported by QMI8658")
        super().__init__()
        self._i2c = i2c
        self.addr = addr
        self.mode = mode
        self.acc_scale = acc_scale
        self.acc_odr = acc_odr
        self.gyro_scale = gyro_scale
        self.gyro_odr = gyro_odr
        self._active = False
        if auto_open:
            self.open()

    @wrap_error_as(QMI8658Error, "QMI8658 open failed", catch=OSError)
    def open(self):
        """Reset, verify, and configure the sensor on its open I2C bus."""
        self.close()
        if not self._i2c.is_opened:
            raise QMI8658Error("QMI8658 I2C bus is not open")
        self._active = True
        try:
            self._write(self._RESET, 0xB0)
            # sleep(2)
            self._update(self._CTRL1, 0x40, 0x40)
            self._update(self._CTRL2, 0x0F, self._ACC_ODR[self.acc_odr])
            self._update(self._CTRL2, 0x70, self._ACC_SCALE[self.acc_scale] << 4)
            self._update(self._CTRL3, 0x0F, self._GYRO_ODR[self.gyro_odr])
            self._update(self._CTRL3, 0x70, self._GYRO_SCALE[self.gyro_scale] << 4)
            enabled = {
                IMUMode.ACC_ONLY: 1,
                IMUMode.GYRO_ONLY: 2,
                IMUMode.DUAL: 3,
            }[self.mode]
            self._update(self._CTRL7, 0x03, enabled)
            if self._read(self._WHO_AM_I) != self._DEVICE_ID:
                raise QMI8658Error("unexpected QMI8658 device ID")
            if self._read(self._CTRL7) & 0x03 != enabled:
                raise QMI8658Error("QMI8658 configuration was not accepted")
            active_odrs = []
            if self.mode is not IMUMode.GYRO_ONLY:
                active_odrs.append(self.acc_odr.value)
            if self.mode is not IMUMode.ACC_ONLY:
                active_odrs.append(self.gyro_odr.value)
            sleep(max(0.08, 1.0 / min(active_odrs)))
        except Exception:
            self._active = False
            raise

    @wrap_error_as(QMI8658Error, "QMI8658 close failed", catch=OSError)
    def close(self):
        """Disable sensor outputs without closing the caller-owned I2C bus."""
        if not self._active:
            return
        try:
            self._write(self._CTRL7, self._read(self._CTRL7) & 0xF0)
            # self._write(self._CTRL1, self._read(self._CTRL1) | 0x01)
        finally:
            self._active = False

    def __enter__(self):
        """Open the sensor if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the sensor when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the sensor is active on an open I2C bus."""
        return self._active and self._i2c.is_opened

    @wrap_error_as(QMI8658Error, "QMI8658 read failed", catch=OSError)
    def read_raw(self):
        """Return uncalibrated axes followed by temperature in Celsius."""
        #print("Status:", self._bus.read_mem(self.addr, 0x2C, 1))
        #sleep(0.05)  # 等待
        #print("Status after delay:", self._bus.read_mem(self.addr, 0x2C, 1))
        raw = unpack("<7h", self._bus.read_mem(self.addr, self._DATA, 14))
        temperature = raw[0] / 256
        acceleration = [value * self.acc_scale.value / (1 << 15) for value in raw[1:4]]
        gyro_scale = self._GYRO_SCALE[self.gyro_scale]
        gyroscope = [value / (1 << (11 - gyro_scale)) for value in raw[4:7]]
        if self.mode is IMUMode.ACC_ONLY:
            return [*acceleration, temperature]
        if self.mode is IMUMode.GYRO_ONLY:
            return [*gyroscope, temperature]
        return [*acceleration, *gyroscope, temperature]

    def read_imu(self, calib_gyro = True, radian = False):
        """Return acceleration and optional calibrated or converted gyro data."""
        sample = self.read_all(calib_gyro, radian)
        return IMUData(acc=sample.acc, gyro=sample.gyro)

    def read_temperature(self):
        """Return the temperature in degrees Celsius."""
        return self.read_all().temperature

    @wrap_error_as(QMI8658Error, "QMI8658 COD calibration failed", catch=OSError)
    def calibrate_cod(self):
        """Run the gyroscope X/Y gain Calibration-On-Demand routine."""
        ctrl7 = self._read(self._CTRL7)
        self._last_cod_gains = None
        try:
            self._write(self._CTRL7, ctrl7 & ~0x03)
            self._execute_ctrl9(self._CTRL_CMD_ON_DEMAND_CALIBRATION, 2.0)
            status = self._read(self._COD_STATUS)
            if status != 0:
                reasons = [
                    reason for mask, reason in self._COD_FAILURES if status & mask
                ]
                raise QMI8658Error(
                    f"COD status 0x{status:02X}: " + "; ".join(reasons)
                )
            self._last_cod_gains = unpack(
                "<3H", self._bus.read_mem(self.addr, self._DVX_L, 6)
            )
            return True
        finally:
            self._write(self._CTRL7, ctrl7)

    def calibrate_all(
        self,
        sample_count = 100,
        interval_ms = 10,
        acc_target = None,
    ):
        """依次执行 COD、陀螺仪零偏和加速度计零偏校准。

        ``acc_target`` 表示预期重力向量，单位为 g。未指定时会自动检测
        最接近竖直方向的传感器轴，因此校准时需要让某个轴基本竖直。
        """
        # 统一记录执行阶段、失败原因、已应用项目以及校准前后的采样结果。
        result = {
            "success": False,
            "stage": "validation",
            "reason": None,
            "applied": {"cod": False, "gyro_offset": False, "acc_offset": False},
            "cod_status": None,
            "cod_gains": None,
            "acc_target_g": None,
            "before": None,
            "after": None,
            "gyro_offset_dps": None,
            "acc_offset_g": None,
        }
        ctrl2 = ctrl3 = ctrl7 = None
        try:
            # 校验采样参数，避免采样数量过少或采样间隔无效。
            if (
                isinstance(sample_count, bool)
                or not isinstance(sample_count, int)
                or sample_count < 20
            ):
                raise ValueError("sample_count must be an integer of at least 20")
            if (
                isinstance(interval_ms, bool)
                or not isinstance(interval_ms, int)
                or interval_ms < 1
            ):
                raise ValueError("interval_ms must be a positive integer")
            if acc_target is not None:
                # 用户指定重力方向时，必须是由三个有限数组成且模长接近 1 g。
                if (
                    not isinstance(acc_target, (tuple, list))
                    or len(acc_target) != 3
                    or any(
                        isinstance(value, bool)
                        or not isinstance(value, (int, float))
                        or not isfinite(value)
                        for value in acc_target
                    )
                ):
                    raise ValueError("acc_target must contain three finite numbers")
                acc_target = tuple(float(value) for value in acc_target)
                target_norm = sqrt(sum(value * value for value in acc_target))
                if not 0.8 <= target_norm <= 1.2:
                    raise ValueError("acc_target magnitude must be close to 1 g")

            # 保存原始配置，以便函数结束时恢复调用前的传感器工作状态。
            result["stage"] = "prepare"
            ctrl2 = self._read(self._CTRL2)
            ctrl3 = self._read(self._CTRL3)
            ctrl7 = self._read(self._CTRL7)

            # 先关闭加速度计和陀螺仪，再统一设置为 250 Hz，保证采样稳定且相互独立。
            self._write(self._CTRL7, ctrl7 & ~0x03)
            self._write(self._CTRL2, ctrl2 & 0xF0 | self._ACC_ODR[IMUAccOdr.ACC_ODR_250])
            self._write(
                self._CTRL3,
                ctrl3 & 0xF0 | self._GYRO_ODR[IMUGyroOdr.GYRO_ODR_250],
            )
            self._write(self._CTRL7, ctrl7 & ~0x03 | 0x03)
            sleep(0.1)

            # 校准前采样并确认设备静止；未指定重力方向时自动判断竖直轴。
            result["stage"] = "stationary_check"
            initial = self._collect_calibration_samples(sample_count, interval_ms)
            stationary_error = self._stationary_error(initial)
            if stationary_error:
                raise QMI8658Error(stationary_error)
            if acc_target is None:
                acc_target = self._detect_acc_target(initial["acc_mean"])
            result["acc_target_g"] = acc_target

            # 执行 COD，校准陀螺仪 X/Y 轴增益，并保存芯片返回的状态和增益值。
            result["stage"] = "cod"
            self.calibrate_cod()
            result["applied"]["cod"] = True
            result["cod_status"] = self._read(self._COD_STATUS)
            result["cod_gains"] = self._last_cod_gains

            # COD 会改变陀螺仪增益，因此必须在 COD 完成后重新采样并计算零偏。
            sleep(0.1)
            # 丢弃前 10 帧过渡数据，再采集用于计算偏置的正式数据。
            self._collect_calibration_samples(10, interval_ms)
            result["stage"] = "offset_sampling"
            before = self._collect_calibration_samples(sample_count, interval_ms)
            result["before"] = before
            stationary_error = self._stationary_error(before)
            if stationary_error:
                raise QMI8658Error(stationary_error)

            # 根据静止时的均值计算陀螺仪和加速度计需要写入的补偿量。
            gyro_offset = tuple(-value for value in before["gyro_mean"])
            acc_offset = tuple(
                target - measured
                for target, measured in zip(acc_target, before["acc_mean"])
            )
            result["gyro_offset_dps"] = gyro_offset
            result["acc_offset_g"] = acc_offset

            # 写入 Host Delta Offset 前先关闭加速度计和陀螺仪。
            self._write(self._CTRL7, ctrl7 & ~0x03)

            # 将陀螺仪偏置转换为 11.5 定点格式并通过 CTRL9 命令应用到芯片。
            result["stage"] = "gyro_offset"
            self._write_calibration_values(gyro_offset, 5)
            self._execute_ctrl9(self._CTRL_CMD_GYRO_HOST_DELTA_OFFSET, 0.5)
            result["applied"]["gyro_offset"] = True
            # 芯片已补偿零偏，清除软件偏置，避免重复补偿。
            self.calib_gyro_data.x = 0.0
            self.calib_gyro_data.y = 0.0
            self.calib_gyro_data.z = 0.0

            # 将加速度计偏置转换为 4.12 定点格式并通过 CTRL9 命令应用到芯片。
            result["stage"] = "acc_offset"
            self._write_calibration_values(acc_offset, 12)
            self._execute_ctrl9(self._CTRL_CMD_ACCEL_HOST_DELTA_OFFSET, 0.5)
            result["applied"]["acc_offset"] = True

            # 重新开启传感器，丢弃过渡数据后采样，验证校准后的残余误差。
            result["stage"] = "verification"
            self._write(self._CTRL7, ctrl7 & ~0x03 | 0x03)
            sleep(0.1)
            self._collect_calibration_samples(10, interval_ms)
            after = self._collect_calibration_samples(max(20, sample_count // 2), interval_ms)
            result["after"] = after
            stationary_error = self._stationary_error(after)
            if stationary_error:
                raise QMI8658Error(f"verification failed: {stationary_error}")
            if max(abs(value) for value in after["gyro_mean"]) > 5.0:
                raise QMI8658Error(
                    "verification failed: residual gyro bias exceeds 5 dps"
                )
            if max(
                abs(measured - target)
                for measured, target in zip(after["acc_mean"], acc_target)
            ) > 0.20:
                raise QMI8658Error(
                    "verification failed: residual accelerometer error exceeds 0.20 g"
                )

            # 所有校准命令及最终误差验证均通过。
            result["success"] = True
            result["stage"] = "complete"
        except Exception as error:
            # 捕获任意阶段的异常，通过 reason 返回失败原因而不是继续抛出。
            if result["stage"] == "cod" and result["cod_status"] is None:
                try:
                    result["cod_status"] = self._read(self._COD_STATUS)
                except Exception:
                    pass
            result["reason"] = str(error) or error.__class__.__name__
        finally:
            # 无论校准成功还是失败，都恢复进入函数前保存的传感器配置。
            if ctrl7 is not None:
                try:
                    self._write(self._CTRL7, ctrl7 & ~0x03)
                    if ctrl2 is not None:
                        self._write(self._CTRL2, ctrl2)
                    if ctrl3 is not None:
                        self._write(self._CTRL3, ctrl3)
                    self._write(self._CTRL7, ctrl7)
                except Exception as error:
                    result["success"] = False
                    result["stage"] = "restore_configuration"
                    result["reason"] = (
                        "failed to restore sensor configuration: "
                        f"{str(error) or error.__class__.__name__}"
                    )
        return result

    def read_all(self, calib_gyro = True, radian = False):
        """Return acceleration, gyro, and temperature data."""
        sample = self.read_raw()
        zero = (0.0, 0.0, 0.0)
        axes = (sample[0], sample[1], sample[2])
        if self.mode is IMUMode.ACC_ONLY:
            acc, gyro, temperature = axes, zero, sample[3]
        elif self.mode is IMUMode.GYRO_ONLY:
            acc, gyro, temperature = zero, axes, sample[3]
        else:
            acc = axes
            gyro = (sample[3], sample[4], sample[5])
            temperature = sample[6]
        return QMI8658Data(
            acc=acc,
            gyro=self._calibrate_gyro(gyro, calib_gyro, radian),
            temperature=temperature,
        )

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise QMI8658Error("QMI8658 is not open")
        return self._i2c

    def _read(self, register):
        """Read one sensor register."""
        return self._bus.read_mem_byte(self.addr, register)

    def _write(self, register, value):
        """Write one sensor register."""
        self._bus.write_mem_byte(self.addr, register, value)

    def _update(self, register, mask, value):
        """Clear the bits selected by ``mask`` and write ``value`` in their place."""
        self._write(register, self._read(register) & ~mask | value)

    def _execute_ctrl9(self, command, timeout_s):
        """Execute one CTRL9 command and complete its acknowledgement handshake."""
        if self._read(self._STATUSINT) & self._STATUSINT_CMD_DONE:
            self._write(self._CTRL9, self._CTRL_CMD_ACK)
            if not self._wait_ctrl9_done(False, 0.1):
                raise QMI8658Error("stale CTRL9 CmdDone flag could not be cleared")
        self._write(self._CTRL9, command)
        command_done = self._wait_ctrl9_done(True, timeout_s)
        try:
            if not command_done:
                raise QMI8658Error(f"CTRL9 command 0x{command:02X} timed out")
        finally:
            if command_done:
                self._write(self._CTRL9, self._CTRL_CMD_ACK)
                if not self._wait_ctrl9_done(False, 0.1):
                    raise QMI8658Error(
                        f"CTRL9 command 0x{command:02X} acknowledgement timed out"
                    )

    def _wait_ctrl9_done(self, expected, timeout_s):
        """Wait until the CTRL9 CmdDone flag matches ``expected``."""
        deadline = monotonic() + timeout_s
        while monotonic() < deadline:
            done = bool(self._read(self._STATUSINT) & self._STATUSINT_CMD_DONE)
            if done is expected:
                return True
            sleep(0.005)
        return False

    def _write_calibration_values(self, values, fraction_bits):
        """Write three signed fixed-point values to CAL1 through CAL3."""
        for index, value in enumerate(values):
            raw = round(value * (1 << fraction_bits))
            if not -0x8000 <= raw <= 0x7FFF:
                raise QMI8658Error(
                    f"calibration value {value} is outside the signed 16-bit range"
                )
            raw &= 0xFFFF
            register = self._CAL1_L + index * 2
            self._write(register, raw & 0xFF)
            self._write(register + 1, raw >> 8)

    def _read_calibration_axes(self):
        """Read temperature and all six axes without applying software bias."""
        raw = unpack("<7h", self._bus.read_mem(self.addr, self._DATA, 14))
        acceleration = tuple(
            value * self.acc_scale.value / (1 << 15) for value in raw[1:4]
        )
        gyro_scale = self._GYRO_SCALE[self.gyro_scale]
        gyroscope = tuple(value / (1 << (11 - gyro_scale)) for value in raw[4:7])
        return acceleration, gyroscope

    def _collect_calibration_samples(self, count, interval_ms):
        """Collect axis means and standard deviations for stationary calibration."""
        acc_samples = []
        gyro_samples = []
        for index in range(count):
            acc, gyro = self._read_calibration_axes()
            acc_samples.append(acc)
            gyro_samples.append(gyro)
            if index + 1 < count:
                sleep(interval_ms / 1_000)

        def summarize(samples):
            means = tuple(
                sum(sample[axis] for sample in samples) / len(samples)
                for axis in range(3)
            )
            deviations = tuple(
                sqrt(
                    sum(
                        (sample[axis] - means[axis]) ** 2 for sample in samples
                    )
                    / len(samples)
                )
                for axis in range(3)
            )
            return means, deviations

        acc_mean, acc_std = summarize(acc_samples)
        gyro_mean, gyro_std = summarize(gyro_samples)
        return {
            "acc_mean": acc_mean,
            "acc_std": acc_std,
            "gyro_mean": gyro_mean,
            "gyro_std": gyro_std,
        }

    @staticmethod
    def _stationary_error(statistics):
        """Return a reason when samples do not look stationary."""
        acc_norm = sqrt(sum(value * value for value in statistics["acc_mean"]))
        if not 0.8 <= acc_norm <= 1.2:
            return f"acceleration magnitude is {acc_norm:.3f} g instead of about 1 g"
        if max(statistics["acc_std"]) > 0.03:
            return "accelerometer vibration exceeds 0.03 g standard deviation"
        if max(statistics["gyro_std"]) > 0.5:
            return "gyroscope motion exceeds 0.5 dps standard deviation"
        if max(abs(value) for value in statistics["gyro_mean"]) > 5.0:
            return "mean angular rate exceeds 5 dps; keep the device stationary"
        return None

    @staticmethod
    def _detect_acc_target(acc_mean):
        """Infer the nearest +/-1 g axis for one-position accelerometer calibration."""
        axis = max(range(3), key=lambda index: abs(acc_mean[index]))
        if abs(acc_mean[axis]) < 0.8:
            raise QMI8658Error(
                "no sensor axis is vertical; pass acc_target or place one axis vertical"
            )
        if max(abs(acc_mean[index]) for index in range(3) if index != axis) > 0.1:
            raise QMI8658Error(
                "sensor is too tilted; pass acc_target or place one axis vertical"
            )
        target = [0.0, 0.0, 0.0]
        target[axis] = 1.0 if acc_mean[axis] >= 0 else -1.0
        return tuple(target)
