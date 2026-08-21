"""LTR390 ambient-light and ultraviolet sensor driver."""


from enum import Enum, IntEnum
from math import isfinite
from time import sleep

from dara.core._error_helpers import wrap_error_as
from dara.device.ls import LS, LSData, LSError, ls_drivers
from dara.peripheral.i2c import I2C


class LTR390Error(LSError):
    """Raised when an LTR390 sensor operation cannot be completed."""


class LTR390Mode(str, Enum):
    """Measurement channel selected by the sensor."""

    AMBIENT_LIGHT = "ambient_light"
    ULTRAVIOLET = "ultraviolet"


class LTR390Gain(IntEnum):
    """Sensor gain multiplier."""

    GAIN_1X = 1
    GAIN_3X = 3
    GAIN_6X = 6
    GAIN_9X = 9
    GAIN_18X = 18


class LTR390Resolution(IntEnum):
    """Measurement resolution in bits."""

    RESOLUTION_20BIT = 20
    RESOLUTION_19BIT = 19
    RESOLUTION_18BIT = 18
    RESOLUTION_17BIT = 17
    RESOLUTION_16BIT = 16
    RESOLUTION_13BIT = 13


class LTR390Data(LSData):
    """LTR390 brightness in lux for ALS mode or UV Index for UVS mode."""

    """Brightness in lux for ambient-light mode or UV Index for UV mode."""


@ls_drivers.register("ltr390")
class LTR390(LS):
    """An LTR390 ambient-light and ultraviolet sensor connected over I2C."""

    _MAIN_CTRL = 0x00
    _MEAS_RATE = 0x04
    _GAIN = 0x05
    _PART_ID = 0x06
    _MAIN_STATUS = 0x07
    _ALS_DATA = 0x0D
    _UVS_DATA = 0x10
    _INT_CFG = 0x19
    _INT_PERSISTENCE = 0x1A
    _THRESHOLD_UPPER = 0x21
    _THRESHOLD_LOWER = 0x24

    _GAIN_VALUES = {gain: index for index, gain in enumerate(LTR390Gain)}
    _RESOLUTION_VALUES = {
        resolution: index for index, resolution in enumerate(LTR390Resolution)
    }
    _INTEGRATION_TIMES_MS = (400.0, 200.0, 100.0, 50.0, 25.0, 12.5)

    def __init__(
        self,
        i2c,
        addr = 0x53,
        mode = LTR390Mode.AMBIENT_LIGHT,
        gain = LTR390Gain.GAIN_1X,
        resolution = LTR390Resolution.RESOLUTION_20BIT,
        threshold = None,
        *,
        window_factor = 1.0,
        auto_open = True,
    ):
        """Create an LTR390, with ``window_factor`` correcting optical loss."""
        if not isinstance(i2c, I2C):
            raise ValueError("i2c must be a Dara I2C instance")
        if isinstance(addr, bool) or not isinstance(addr, int) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        if (
            isinstance(window_factor, bool)
            or not isinstance(window_factor, (int, float))
            or not isfinite(window_factor)
            or window_factor <= 0
        ):
            raise ValueError("window_factor must be a positive finite number")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self._i2c = i2c
        self.addr = addr
        self._threshold = None
        self.window_factor = float(window_factor)
        self._active = False
        self.reset(mode, gain, resolution, threshold)
        if auto_open:
            self.open()

    @wrap_error_as(LTR390Error, "LTR390 open failed", catch=OSError)
    def open(self):
        """Verify, reset, and enable the sensor on its open I2C bus."""
        self.close()
        if not self._i2c.is_opened:
            raise LTR390Error("LTR390 I2C bus is not open")
        self._active = True
        try:
            if self._read(self._PART_ID) >> 4 != 0x0B:
                raise LTR390Error("unexpected LTR390 part ID")
            self._soft_reset()
            self._apply_configuration()
            self._set_enabled(True)
            if not self._is_enabled():
                raise LTR390Error("LTR390 did not enable")
        except Exception:
            self._active = False
            raise

    @wrap_error_as(LTR390Error, "LTR390 close failed", catch=OSError)
    def close(self):
        """Disable the sensor without closing its caller-owned I2C bus."""
        if not self._active:
            return
        try:
            self._set_enabled(False)
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

    def reset(
        self,
        mode = None,
        gain = None,
        resolution = None,
        threshold = None,
    ):
        """Update non-``None`` settings and reinitialize an open sensor."""
        if mode is not None and not isinstance(mode, LTR390Mode):
            raise ValueError("mode must be an LTR390Mode or None")
        if gain is not None and not isinstance(gain, LTR390Gain):
            raise ValueError("gain must be an LTR390Gain or None")
        if resolution is not None and not isinstance(resolution, LTR390Resolution):
            raise ValueError("resolution must be an LTR390Resolution or None")
        if threshold is not None:
            if not isinstance(threshold, tuple) or len(threshold) != 2:
                raise ValueError("threshold must be a (lower, upper) tuple or None")
            lower, upper = threshold
            for value in threshold:
                if (
                    isinstance(value, bool)
                    or not isinstance(value, int)
                    or not 0 <= value <= 0xFFFFFF
                ):
                    raise ValueError("threshold values must be 24-bit integers")
            if lower > upper:
                raise ValueError("lower threshold must not exceed upper threshold")
        if mode is not None:
            self._mode = mode
        if gain is not None:
            self._gain = gain
        if resolution is not None:
            self._resolution = resolution
        if threshold is not None:
            self._threshold = threshold
        if self.is_opened:
            self.open()

    def _soft_reset(self):
        """Soft-reset the sensor and verify that the reset bit clears."""
        control = self._read(self._MAIN_CTRL)
        try:
            self._write(self._MAIN_CTRL, control | 0x10)
        except OSError:
            pass  # The sensor may reset before acknowledging the command.
        sleep(0.01)
        if self._read(self._MAIN_CTRL) & 0x10:
            raise LTR390Error("LTR390 reset did not complete")

    def _apply_configuration(self):
        """Apply configured mode, gain, resolution, and thresholds."""
        self._update(
            self._MAIN_CTRL,
            0x08,
            0x08 if self._mode is LTR390Mode.ULTRAVIOLET else 0,
        )
        self._update(self._GAIN, 0x07, self._GAIN_VALUES[self._gain])
        self._update(
            self._MEAS_RATE,
            0x70,
            self._RESOLUTION_VALUES[self._resolution] << 4,
        )
        if self._threshold is not None:
            lower, upper = self._threshold
            self._bus.write_mem(
                self.addr, self._THRESHOLD_LOWER, lower.to_bytes(3, "little")
            )
            self._bus.write_mem(
                self.addr, self._THRESHOLD_UPPER, upper.to_bytes(3, "little")
            )

    def _set_enabled(self, enabled):
        """Enable or disable sensor measurements."""
        self._update(self._MAIN_CTRL, 0x02, 0x02 if enabled else 0)

    def _is_enabled(self):
        """Return whether sensor measurements are enabled."""
        return bool(self._read(self._MAIN_CTRL) & 0x02)

    @wrap_error_as(LTR390Error, "LTR390 interrupt update failed", catch=OSError)
    def configure_interrupt(
        self,
        enabled,
        source,
        persistence = 0,
    ):
        """Configure threshold interrupt enable, source, and persistence."""
        if not isinstance(enabled, bool):
            raise ValueError("enabled must be a boolean")
        if not isinstance(source, LTR390Mode):
            raise ValueError("source must be an LTR390Mode")
        if (
            isinstance(persistence, bool)
            or not isinstance(persistence, int)
            or not 0 <= persistence <= 15
        ):
            raise ValueError("persistence must be an integer from 0 to 15")
        source_value = 1 if source is LTR390Mode.AMBIENT_LIGHT else 3
        self._update(self._INT_CFG, 0x34, source_value << 4 | enabled << 2)
        self._update(self._INT_PERSISTENCE, 0xF0, persistence << 4)

    @wrap_error_as(LTR390Error, "LTR390 status read failed", catch=OSError)
    def is_data_ready(self):
        """Return whether a new sample is available."""
        return bool(self._read(self._MAIN_STATUS) & 0x08)

    @wrap_error_as(LTR390Error, "LTR390 brightness read failed", catch=OSError)
    def read_brightness(self):
        """Return illuminance in lux or, in ultraviolet mode, UV Index."""
        raw = self.read_raw()
        gain = self._gain.value
        integration_time = self._INTEGRATION_TIMES_MS[
            self._RESOLUTION_VALUES[self._resolution]
        ]
        if self._mode is LTR390Mode.AMBIENT_LIGHT:
            return (
                0.6
                * raw
                / (gain * integration_time / 100)
                * self.window_factor
            )
        sensitivity = 2300 * (gain / 18) * (integration_time / 400)
        return raw / sensitivity * self.window_factor

    @wrap_error_as(LTR390Error, "LTR390 raw read failed", catch=OSError)
    def read_raw(self):
        """Return the unitless raw count from the active measurement mode."""
        register = (
            self._UVS_DATA
            if self._mode is LTR390Mode.ULTRAVIOLET
            else self._ALS_DATA
        )
        return self._read_data(register)

    def read_all(self):
        """Return brightness in lux or, in ultraviolet mode, UV Index."""
        return LTR390Data(brightness=self.read_brightness())

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise LTR390Error("LTR390 is not open")
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

    def _read_data(self, register):
        """Read one little-endian 24-bit data register."""
        return int.from_bytes(self._bus.read_mem(self.addr, register, 3), "little")
