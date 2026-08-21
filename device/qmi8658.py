"""QMI8658 six-axis inertial measurement unit driver."""


from struct import unpack
from time import sleep

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
    _DATA = 0x33
    _RESET = 0x60
    _DEVICE_ID = 0x05

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
            sleep(2)
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
