"""MMC5603 and MMC5613 magnetometer driver."""


from math import nan
from time import monotonic, sleep

from dara.core._error_helpers import wrap_error_as
from dara.device.mag import Mag, MagData, MagError, mag_drivers
from dara.device.ts import TS, TSData, TSError, ts_drivers
from dara.peripheral.i2c import I2C


class MMC56X3Error(TSError, MagError):
    """Raised when an MMC56X3 operation cannot be completed."""


class MMC56X3Data(TSData, MagData):
    """Complete magnetic-field and temperature sample."""

    def __init__(self, mag, temperature):
        """Initialize a complete MMC56X3 sample."""
        self.mag = mag
        self.temperature = temperature


class MMC56X3Calibration:
    """Three-axis hard-iron offset and axis scale calibration."""

    def __init__(self, offset, scale, minimum, maximum, sample_count):
        """Initialize a magnetometer calibration result."""
        self.offset = offset
        self.scale = scale
        self.minimum = minimum
        self.maximum = maximum
        self.sample_count = sample_count


@mag_drivers.register("mmc56x3")
@ts_drivers.register("mmc56x3")
class MMC56X3(Mag, TS):
    """An MMC5603 or MMC5613 magnetometer connected over I2C."""

    _OUT_X = 0x00
    _OUT_TEMP = 0x09
    _ODR = 0x1A
    _CTRL0 = 0x1B
    _CTRL1 = 0x1C
    _CTRL2 = 0x1D
    _STATUS = 0x18
    _PRODUCT_ID = 0x39
    _CONTINUOUS = 0x10
    _HIGH_POWER = 0x80
    _POLL_INTERVAL = 0.005
    _POLL_TIMEOUT = 0.1

    def __init__(
        self,
        i2c = None,
        addr = 0x30,
        *,
        auto_open = True,
    ):
        """Create an MMC56X3 and optionally initialize it on an I2C bus.

        When ``i2c`` is omitted, I2C0 is opened here rather than while this
        module is imported.  This preserves the convenient default without
        making ``import dara.device.mmc56x3`` touch board hardware.
        """
        if i2c is None:
            i2c = I2C(0)
        if not isinstance(i2c, I2C):
            raise ValueError("i2c must be an I2C instance")
        if not isinstance(addr, int) or isinstance(addr, bool) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self._i2c = i2c
        self.addr = addr
        self._active = False
        self._ctrl2 = 0
        self._data_rate = 0
        self._mag_calibration = None
        if auto_open:
            self.open()

    @wrap_error_as(MMC56X3Error, "MMC56X3 open failed", catch=OSError)
    def open(self):
        """Verify and initialize the sensor on its open I2C bus."""
        self.close()
        if not self._i2c.is_opened:
            raise MMC56X3Error("MMC56X3 I2C bus is not open")
        self._active = True
        try:
            if self._read(self._PRODUCT_ID) not in (0x00, 0x10):
                raise MMC56X3Error("unexpected MMC56X3 product ID")
            self.reset()
        except Exception:
            self._active = False
            raise

    @wrap_error_as(MMC56X3Error, "MMC56X3 close failed", catch=OSError)
    def close(self):
        """Disable continuous sampling without closing the caller-owned bus."""
        if not self._active:
            return
        try:
            self._ctrl2 &= ~self._CONTINUOUS
            self._write(self._CTRL2, self._ctrl2)
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

    @wrap_error_as(MMC56X3Error, "MMC56X3 reset failed", catch=OSError)
    def reset(self):
        """Reset the sensor, pulse its coils, and select one-shot mode."""
        self._write(self._CTRL1, 0x80)
        sleep(0.02)
        self._ctrl2 = 0
        self._data_rate = 0
        self.magnet_set_reset()
        self.set_continuous_mode(False)

    @wrap_error_as(MMC56X3Error, "MMC56X3 set/reset failed", catch=OSError)
    def magnet_set_reset(self):
        """Pulse the sensor set and reset coils to clear magnetic offset."""
        self._write(self._CTRL0, 0x08)
        sleep(0.001)
        self._write(self._CTRL0, 0x10)
        sleep(0.001)

    @wrap_error_as(MMC56X3Error, "MMC56X3 continuous mode failed", catch=OSError)
    def set_continuous_mode(self, mode):
        """Enable or disable continuous magnetic sampling."""
        if not isinstance(mode, bool):
            raise ValueError("mode must be a boolean")
        if mode:
            self._write(self._CTRL0, 0x80)
            self._ctrl2 |= self._CONTINUOUS
        else:
            self._ctrl2 &= ~self._CONTINUOUS
        self._write(self._CTRL2, self._ctrl2)

    def is_continuous_mode(self):
        """Return whether continuous magnetic sampling is enabled."""
        return bool(self._ctrl2 & self._CONTINUOUS)

    @wrap_error_as(MMC56X3Error, "MMC56X3 temperature read failed", catch=OSError)
    def read_temperature(self):
        """Return temperature in Celsius, or NaN during continuous sampling."""
        if self.is_continuous_mode():
            return nan
        self._write(self._CTRL0, 0x02)
        self._wait_ready(0x80, "temperature measurement")
        return self._read(self._OUT_TEMP) * 0.8 - 75

    @wrap_error_as(MMC56X3Error, "MMC56X3 data rate update failed", catch=OSError)
    def set_data_rate(self, rate):
        """Set the output rate to 0 through 255 Hz, or high-power 1000 Hz."""
        if isinstance(rate, bool) or not isinstance(rate, int) or rate < 0:
            raise ValueError("rate must be a non-negative integer")
        effective_rate = rate if rate <= 255 else 1000
        self._write(self._ODR, 255 if effective_rate == 1000 else effective_rate)
        if effective_rate == 1000:
            self._ctrl2 |= self._HIGH_POWER
        else:
            self._ctrl2 &= ~self._HIGH_POWER
        self._write(self._CTRL2, self._ctrl2)
        self._data_rate = effective_rate

    @property
    def data_rate(self):
        """Return the cached effective output data rate in hertz."""
        return self._data_rate

    @wrap_error_as(MMC56X3Error, "MMC56X3 read failed", catch=OSError)
    def read_mag(self, calibrated = False):
        """Return raw or calibrated magnetic field axes in microteslas."""
        if not isinstance(calibrated, bool):
            raise ValueError("calibrated must be a boolean")
        if not self.is_continuous_mode():
            self._write(self._CTRL0, 0x01)
            self._wait_ready(0x40, "magnetic measurement")
        sample = self._bus.read_mem(self.addr, self._OUT_X, 9)
        decoded = tuple(
            (
                (
                    (sample[index] << 12)
                    | (sample[index + 1] << 4)
                    | (sample[6 + index // 2] >> 4)
                )
                - (1 << 19)
            )
            * 0.00625
            for index in (0, 2, 4)
        )
        mag = (decoded[0], decoded[1], decoded[2])
        if not calibrated:
            return mag
        if self._mag_calibration is None:
            raise MMC56X3Error("magnetometer has not been calibrated")
        return tuple(
            (value - self._mag_calibration.offset[index])
            * self._mag_calibration.scale[index]
            for index, value in enumerate(mag)
        )

    @wrap_error_as(MMC56X3Error, "MMC56X3 calibration failed", catch=OSError)
    def calibrate_mag(self, time_ms, interval_ms = 20):
        """Collect rotated samples, activate calibration, and return its result.

        Rotate the sensor through all orientations while this function runs.
        The result compensates hard-iron offset and per-axis scale differences.
        """
        if isinstance(time_ms, bool) or not isinstance(time_ms, int) or time_ms <= 0:
            raise ValueError("time_ms must be a positive integer")
        if (
            isinstance(interval_ms, bool)
            or not isinstance(interval_ms, int)
            or interval_ms < 0
        ):
            raise ValueError("interval_ms must be a non-negative integer")

        self.magnet_set_reset()
        minimum = [float("inf"), float("inf"), float("inf")]
        maximum = [float("-inf"), float("-inf"), float("-inf")]
        sample_count = 0
        deadline = monotonic() + time_ms / 1_000
        while True:
            sample = self.read_mag(calibrated=False)
            for index, value in enumerate(sample):
                minimum[index] = min(minimum[index], value)
                maximum[index] = max(maximum[index], value)
            sample_count += 1
            if monotonic() >= deadline:
                break
            if interval_ms:
                sleep(interval_ms / 1_000)

        radius = [
            (maximum[index] - minimum[index]) / 2
            for index in range(3)
        ]
        if any(value <= 0 for value in radius):
            raise MMC56X3Error(
                "insufficient axis movement during magnetometer calibration"
            )
        average_radius = sum(radius) / 3
        calibration = MMC56X3Calibration(
            offset=tuple(
                (maximum[index] + minimum[index]) / 2
                for index in range(3)
            ),
            scale=tuple(average_radius / value for value in radius),
            minimum=tuple(minimum),
            maximum=tuple(maximum),
            sample_count=sample_count,
        )
        self._mag_calibration = calibration
        return calibration

    @property
    def mag_calibration(self):
        """Return the active magnetometer calibration, if any."""
        return self._mag_calibration

    def read_all(self, calibrated = False):
        """Return raw or calibrated magnetic field and temperature data."""
        return MMC56X3Data(
            mag=self.read_mag(calibrated=calibrated),
            temperature=self.read_temperature(),
        )

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise MMC56X3Error("MMC56X3 is not open")
        return self._i2c

    def _read(self, register):
        """Read one sensor register."""
        return self._bus.read_mem_byte(self.addr, register)

    def _write(self, register, value):
        """Write one sensor register."""
        self._bus.write_mem_byte(self.addr, register, value)

    def _wait_ready(self, mask, operation):
        """Poll a status bit until it is ready or the fixed deadline expires."""
        deadline = monotonic() + self._POLL_TIMEOUT
        while not self._read(self._STATUS) & mask:
            if monotonic() >= deadline:
                raise MMC56X3Error(f"{operation} timed out")
            sleep(self._POLL_INTERVAL)
