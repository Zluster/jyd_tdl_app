"""AHT20 temperature and humidity sensor driver."""


from math import isfinite
from time import sleep

from dara.core._error_helpers import wrap_error_as
from dara.device.hs import HSData, HSError
from dara.device.ts import TSData, TSError
from dara.peripheral.i2c import I2C


class AHT20Error(TSError, HSError):
    """Raised when an AHT20 sensor operation cannot be completed."""


class AHT20Data(HSData, TSData):
    """Temperature and relative humidity from one AHT20 sample."""

    def __init__(self, temperature, humidity):
        """Initialize an AHT20 sample."""
        self.temperature = temperature
        self.humidity = humidity


class AHT20:
    """An AHT20 temperature and humidity sensor on an I2C bus."""

    _POWER_ON_DELAY = 0.5
    _MEASUREMENT_DELAY = 0.085
    _CALIBRATION_REGISTERS = (0x1B, 0x1C, 0x1E)

    def __init__(
        self,
        i2c_bus,
        addr = 0x38,
        *,
        temperature_offset = 0.0,
        humidity_offset = 0.0,
        auto_open = True,
    ):
        """Create an AHT20 on an :class:`I2C` object or mapped bus ID."""
        if isinstance(i2c_bus, I2C):
            self._i2c = i2c_bus
        elif isinstance(i2c_bus, (int, str)) and not isinstance(i2c_bus, bool):
            self._i2c = I2C(i2c_bus, auto_open=False)
        else:
            raise ValueError("i2c must be an I2C object or mapped I2C identifier")
        if isinstance(addr, bool) or not isinstance(addr, int) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        for name, value in (
            ("temperature_offset", temperature_offset),
            ("humidity_offset", humidity_offset),
        ):
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not isfinite(value)
            ):
                raise ValueError(f"{name} must be a finite number")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self.addr = addr
        self.temperature_offset = float(temperature_offset)
        self.humidity_offset = float(humidity_offset)
        self._active = False
        self._opened_i2c_here = False
        if auto_open:
            self.open()

    def __enter__(self):
        """Open the sensor if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the sensor when leaving a ``with`` statement."""
        self.close()

    @wrap_error_as(AHT20Error, "AHT20 open failed", catch=OSError)
    def open(self):
        """Open I2C if needed and initialize the sensor."""
        self.close()
        if not self._i2c.is_opened:
            self._i2c.open()
            self._opened_i2c_here = True
        self._active = True
        try:
            sleep(self._POWER_ON_DELAY)
            if self._i2c.read_byte(self.addr) & 0x18 != 0x18:
                for register in self._CALIBRATION_REGISTERS:
                    self._reset_calibration_register(register)
            sleep(0.01)
        except Exception:
            self._active = False
            if self._opened_i2c_here:
                self._i2c.close()
                self._opened_i2c_here = False
            raise

    def close(self):
        """Deactivate the sensor without closing a caller-owned I2C bus."""
        self._active = False
        if self._opened_i2c_here:
            self._i2c.close()
            self._opened_i2c_here = False

    @property
    def is_opened(self):
        """Return whether the sensor is active on an open I2C bus."""
        return self._active and self._i2c.is_opened

    def read_temperature(self):
        """Return the temperature in degrees Celsius."""
        return self.read_all().temperature

    def read_humidity(self):
        """Return the relative humidity as a percentage."""
        return self.read_all().humidity

    @wrap_error_as(AHT20Error, "AHT20 read failed", catch=OSError)
    def read_all(self):
        """Return temperature and relative humidity from one measurement."""
        bus = self._bus
        bus.write(self.addr, b"\xAC\x33\x00")
        sleep(self._MEASUREMENT_DELAY)
        payload = bus.read(self.addr, 7)
        if payload[0] & 0x80:
            raise AHT20Error("AHT20 data is not ready")
        if self._crc8(payload[:6]) != payload[6]:
            raise AHT20Error("invalid AHT20 CRC")

        humidity_raw = (payload[1] << 12) | (payload[2] << 4) | (payload[3] >> 4)
        temperature_raw = ((payload[3] & 0x0F) << 16) | (payload[4] << 8) | payload[5]
        return AHT20Data(
            temperature=temperature_raw / 2**20 * 200 - 50 + self.temperature_offset,
            humidity=humidity_raw / 2**20 * 100 + self.humidity_offset,
        )

    @property
    def _bus(self):
        """Return the open bus or raise the device-level closed error."""
        if not self.is_opened:
            raise AHT20Error("AHT20 is not open")
        return self._i2c

    def _reset_calibration_register(self, register):
        """Restore one calibration register using the reference sequence."""
        self._i2c.write(self.addr, bytes((register, 0, 0)))
        sleep(0.005)
        calibration = self._i2c.read(self.addr, 3)
        sleep(0.01)
        self._i2c.write(
            self.addr, bytes((0xB0 | register, calibration[1], calibration[2]))
        )

    @staticmethod
    def _crc8(data):
        """Return the AHT20 CRC-8 value for ``data``."""
        crc = 0xFF
        for byte in data:
            crc ^= byte
            for _ in range(8):
                crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else crc << 1
        return crc
