# ruff: noqa: E402
"""AHT20 temperature and humidity sensor driver."""

from dara.device.hs import HS, HSData, HSError, hs_drivers
from dara.device.ts import TS, TSData, TSError, ts_drivers
from dara.peripheral.i2c import I2C

class AHT20Error(TSError, HSError):
    """Raised when an AHT20 sensor operation cannot be completed."""
    ...


class AHT20Data(HSData, TSData):
    """Temperature and relative humidity from one AHT20 sample."""
    def __init__(self, temperature: float, humidity: float) -> None:
        """Initialize an AHT20 sample."""
        ...



@ts_drivers.register("aht20")
@hs_drivers.register("aht20")
class AHT20(TS, HS):
    """An AHT20 temperature and humidity sensor on an I2C bus."""
    _POWER_ON_DELAY = ...
    _MEASUREMENT_DELAY = ...
    _CALIBRATION_REGISTERS = ...
    def __init__(self, i2c: I2C, addr: int = ..., *, temperature_offset: float = ..., humidity_offset: float = ..., auto_open: bool = ...) -> None:
        """Create an AHT20 on ``i2c`` and optionally initialize it."""
        ...

    def __enter__(self) -> AHT20:
        """Open the sensor if needed and return it for a ``with`` statement."""
        ...

    def __exit__(self, *args: object) -> None:
        """Close the sensor when leaving a ``with`` statement."""
        ...

    def open(self) -> None:
        """Initialize the sensor without taking ownership of its I2C bus."""
        ...

    def close(self) -> None:
        """Deactivate the sensor without closing its caller-owned I2C bus."""
        ...

    @property
    def is_opened(self) -> bool:
        """Return whether the sensor is active on an open I2C bus."""
        ...

    def read_temperature(self) -> float:
        """Return the temperature in degrees Celsius."""
        ...

    def read_humidity(self) -> float:
        """Return the relative humidity as a percentage."""
        ...

    def read_all(self) -> AHT20Data:
        """Return temperature and relative humidity from one measurement."""
        ...
    @property
    def _bus(self) -> I2C: ...
    def _reset_calibration_register(self, register: int) -> None: ...
    @staticmethod
    def _crc8(data: bytes) -> int: ...
