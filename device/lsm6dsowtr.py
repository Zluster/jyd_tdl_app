"""LSM6DSOWTR six-axis inertial measurement unit driver."""


from struct import unpack

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


class LSM6DSOWTRError(TSError, IMUError):
    """Raised when an LSM6DSOWTR operation cannot be completed."""


class LSM6DSOWTRData(TSData, IMUData):
    """Complete accelerometer, gyroscope, and temperature sample."""

    def __init__(self, acc, gyro, temperature):
        """Initialize a complete LSM6DSOWTR sample."""
        self.acc = acc
        self.gyro = gyro
        self.temperature = temperature


@imu_drivers.register("lsm6dsowtr")
@ts_drivers.register("lsm6dsowtr")
class LSM6DSOWTR(IMU, TS):
    """An LSM6DSOWTR accelerometer and gyroscope connected over I2C."""

    _WHO_AM_I = 0x0F
    _CTRL1_XL = 0x10
    _CTRL2_G = 0x11
    _CTRL3_C = 0x12
    _DATA = 0x20
    _DEVICE_ID = 0x6C
    _GRAVITY = 9.80665

    _ACC_SCALE = {
        IMUAccScale.ACC_SCALE_2G: (0x00, 0.061),
        IMUAccScale.ACC_SCALE_4G: (0x08, 0.122),
        IMUAccScale.ACC_SCALE_8G: (0x0C, 0.244),
        IMUAccScale.ACC_SCALE_16G: (0x04, 0.488),
    }
    _GYRO_SCALE = {
        IMUGyroScale.GYRO_SCALE_125DPS: (0x02, 4.375),
        IMUGyroScale.GYRO_SCALE_250DPS: (0x00, 8.75),
        IMUGyroScale.GYRO_SCALE_500DPS: (0x04, 17.5),
        IMUGyroScale.GYRO_SCALE_1000DPS: (0x08, 35.0),
        IMUGyroScale.GYRO_SCALE_2000DPS: (0x0C, 70.0),
    }
    _ODR = {
        12.5: 1,
        26.0: 2,
        52.0: 3,
        104.0: 4,
        208.0: 5,
        416.0: 6,
        833.0: 7,
    }

    def __init__(
        self,
        i2c,
        addr = 0x6B,
        mode = IMUMode.DUAL,
        acc_scale = IMUAccScale.ACC_SCALE_2G,
        acc_odr = IMUAccOdr.ACC_ODR_104,
        gyro_scale = IMUGyroScale.GYRO_SCALE_250DPS,
        gyro_odr = IMUGyroOdr.GYRO_ODR_104,
        *,
        auto_open = True,
    ):
        """Create an LSM6DSOWTR on a caller-owned bus and optionally initialize it."""
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
            ("acc_odr", acc_odr, self._ODR),
            ("gyro_scale", gyro_scale, self._GYRO_SCALE),
            ("gyro_odr", gyro_odr, self._ODR),
        ):
            if value not in supported and value.value not in supported:
                raise ValueError(f"{name} is not supported by LSM6DSOWTR")

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

    @wrap_error_as(LSM6DSOWTRError, "LSM6DSOWTR open failed", catch=OSError)
    def open(self):
        """Verify and configure the sensor on its open I2C bus."""
        self.close()
        if not self._i2c.is_opened:
            raise LSM6DSOWTRError("LSM6DSOWTR I2C bus is not open")
        self._active = True
        try:
            if self._read(self._WHO_AM_I) != self._DEVICE_ID:
                raise LSM6DSOWTRError("unexpected LSM6DSOWTR device ID")
            self._write(self._CTRL3_C, 0x44)
            acc_odr = (
                0 if self.mode is IMUMode.GYRO_ONLY
                else self._ODR[self.acc_odr.value]
            )
            gyro_odr = (
                0 if self.mode is IMUMode.ACC_ONLY
                else self._ODR[self.gyro_odr.value]
            )
            self._write(
                self._CTRL1_XL,
                acc_odr << 4 | self._ACC_SCALE[self.acc_scale][0],
            )
            self._write(
                self._CTRL2_G,
                gyro_odr << 4 | self._GYRO_SCALE[self.gyro_scale][0],
            )
        except Exception:
            self._active = False
            raise

    @wrap_error_as(LSM6DSOWTRError, "LSM6DSOWTR close failed", catch=OSError)
    def close(self):
        """Disable sensor outputs without closing the caller-owned I2C bus."""
        if not self._active:
            return
        try:
            self._write(self._CTRL1_XL, 0)
            self._write(self._CTRL2_G, 0)
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

    @wrap_error_as(LSM6DSOWTRError, "LSM6DSOWTR read failed", catch=OSError)
    def read_raw(self):
        """Return uncalibrated axes followed by temperature in Celsius."""
        raw = unpack("<7h", self._bus.read_mem(self.addr, self._DATA, 14))
        temperature = raw[0] / 256 + 25
        gyroscope = [
            value * self._GYRO_SCALE[self.gyro_scale][1] / 1_000
            for value in raw[1:4]
        ]
        acc_sensitivity = self._ACC_SCALE[self.acc_scale][1]
        acceleration = [
            value * acc_sensitivity * self._GRAVITY / 1_000 for value in raw[4:7]
        ]
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
        return LSM6DSOWTRData(
            acc=acc,
            gyro=self._calibrate_gyro(gyro, calib_gyro, radian),
            temperature=temperature,
        )

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise LSM6DSOWTRError("LSM6DSOWTR is not open")
        return self._i2c

    def _read(self, register):
        """Read one sensor register."""
        return self._bus.read_mem_byte(self.addr, register)

    def _write(self, register, value):
        """Write one sensor register."""
        self._bus.write_mem_byte(self.addr, register, value)
