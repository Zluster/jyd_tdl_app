"""DHT11 and DHT22 temperature and humidity sensor driver."""


from enum import Enum
from math import isfinite
from time import monotonic, sleep

from dara.core._error_helpers import wrap_error_as
from dara.device.hs import HS, HSData, HSError, hs_drivers
from dara.device.ts import TS, TSData, TSError, ts_drivers
from dara.peripheral.gpio import (
    GPIO,
    GPIODirection,
    GPIODrive,
    GPIOEdge,
    GPIOPull,
)


class DHTType(str, Enum):
    """DHT sensor models supported by the driver."""

    DHT11 = "dht11"
    DHT22 = "dht22"


class DHTError(TSError, HSError):
    """Raised when a DHT sensor operation cannot be completed."""


class DHTData(HSData, TSData):
    """Temperature and relative humidity from one DHT sample."""

    def __init__(self, temperature, humidity):
        """Initialize a DHT sample."""
        self.temperature = temperature
        self.humidity = humidity


@ts_drivers.register("dht")
@hs_drivers.register("dht")
class DHT(TS, HS):
    """A DHT11 or DHT22 sensor connected to a GPIO line."""

    _EDGE_TIMEOUT = 0.001
    _STABILIZE_DELAY = 0.5
    _START_LOW_DELAY = {DHTType.DHT11: 0.018, DHTType.DHT22: 0.001}
    _MIN_READ_INTERVAL = {DHTType.DHT11: 1.0, DHTType.DHT22: 2.0}

    def __init__(
        self,
        gpio,
        dht_type = DHTType.DHT22,
        *,
        temperature_offset = 0.0,
        humidity_offset = 0.0,
        pulse_threshold_us = 50.0,
        auto_open = True,
    ):
        """Create a DHT sensor on a GPIO identifier and optionally activate it."""
        if isinstance(gpio, bool) or not isinstance(gpio, (int, str)):
            raise ValueError("gpio must be a GPIO identifier")
        if not isinstance(dht_type, DHTType):
            raise ValueError("dht_type must be a DHTType value")
        for name, value in (
            ("temperature_offset", temperature_offset),
            ("humidity_offset", humidity_offset),
            ("pulse_threshold_us", pulse_threshold_us),
        ):
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not isfinite(value)
            ):
                raise ValueError(f"{name} must be a finite number")
        if pulse_threshold_us <= 0:
            raise ValueError("pulse_threshold_us must be positive")
        if not isinstance(auto_open, bool):
            raise ValueError("auto_open must be a boolean")

        self._gpio = GPIO(gpio, auto_open=False)
        self.dht_type = dht_type
        self.temperature_offset = float(temperature_offset)
        self.humidity_offset = float(humidity_offset)
        self.pulse_threshold_us = float(pulse_threshold_us)
        self._active = False
        self._sample = None
        self._last_attempt = float("-inf")
        self._last_attempt_succeeded = False
        if auto_open:
            self.open()

    @wrap_error_as(DHTError, "DHT open failed", catch=OSError)
    def open(self):
        """Open the GPIO and activate the sensor."""
        self.close()
        self._gpio.open()
        self._active = True

    @wrap_error_as(DHTError, "DHT close failed", catch=OSError)
    def close(self):
        """Deactivate the sensor and close its GPIO."""
        try:
            self._gpio.close()
        finally:
            self._active = False
            self._sample = None

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
        """Return whether the sensor and its GPIO are open."""
        return self._active and self._gpio.is_opened

    def read_temperature(self):
        """Return the temperature in degrees Celsius."""
        return self._read_sample().temperature

    def read_humidity(self):
        """Return the relative humidity as a percentage."""
        return self._read_sample().humidity

    def read_all(self):
        """Return temperature and relative humidity from one sample."""
        return self._read_sample()

    def _read_sample(self):
        """Return a cached sample or perform one model-rate-limited transaction."""
        if not self.is_opened:
            raise DHTError("DHT is not open")

        now = monotonic()
        if now - self._last_attempt < self._MIN_READ_INTERVAL[self.dht_type]:
            if self._last_attempt_succeeded and self._sample is not None:
                return self._sample
            raise DHTError("DHT retry requested before the sensor read interval")

        self._last_attempt = now
        self._last_attempt_succeeded = False
        sample = self._read_physical_sample()
        self._sample = sample
        self._last_attempt_succeeded = True
        return sample

    @wrap_error_as(DHTError, "DHT read failed", catch=OSError)
    def _read_physical_sample(self):
        """Perform and decode one DHT wire transaction."""
        self._gpio.reset(
            GPIODirection.HIGH,
            GPIOPull.UP,
            GPIOEdge.BOTH,
            GPIODrive.DEFAULT,
            False,
        )
        try:
            sleep(self._STABILIZE_DELAY)
            self._gpio.low()
            sleep(self._START_LOW_DELAY[self.dht_type])
            self._gpio.high()
            self._gpio.reset(
                GPIODirection.IN,
                GPIOPull.UP,
                GPIOEdge.BOTH,
                GPIODrive.DEFAULT,
                False,
            )
            payload = self._decode_pulses(self._receive_high_pulses())
            if sum(payload[:4]) & 0xFF != payload[4]:
                raise DHTError("invalid DHT checksum")
            return self._decode_payload(payload)
        finally:
            if self._gpio.is_opened:
                self._gpio.reset(GPIODirection.IN, GPIOPull.UP)

    def _receive_high_pulses(self):
        """Synchronize with the response preamble and return 40 high durations."""
        first = self._next_edge()
        if first.edge is GPIOEdge.FALLING:
            self._expect_edge(GPIOEdge.RISING)
            self._expect_edge(GPIOEdge.FALLING)
        elif first.edge is GPIOEdge.RISING:
            self._expect_edge(GPIOEdge.FALLING)
        else:
            raise DHTError("invalid DHT response edge")

        durations = []
        for _ in range(40):
            rising = self._expect_edge(GPIOEdge.RISING)
            falling = self._expect_edge(GPIOEdge.FALLING)
            duration = falling.timestamp - rising.timestamp
            if duration <= 0:
                raise DHTError("invalid DHT pulse timestamps")
            durations.append(duration)
        return durations

    def _next_edge(self):
        """Return the next edge or raise when the bounded wait expires."""
        if not self._gpio.poll(self._EDGE_TIMEOUT):
            raise DHTError("DHT pulse timed out")
        return self._gpio.read_event()

    def _expect_edge(self, edge):
        """Return the next edge after validating its type."""
        event = self._next_edge()
        if event.edge is not edge:
            raise DHTError(f"expected DHT {edge.value} edge")
        return event

    def _decode_pulses(self, durations):
        """Convert 40 nanosecond pulse durations to five bytes."""
        threshold_ns = self.pulse_threshold_us * 1_000
        payload = bytearray(5)
        for index, duration in enumerate(durations):
            byte = index // 8
            payload[byte] = payload[byte] << 1 | (duration > threshold_ns)
        return bytes(payload)

    def _decode_payload(self, payload):
        """Decode model-specific humidity and Celsius temperature bytes."""
        if self.dht_type is DHTType.DHT11:
            humidity = payload[0] + payload[1] / 10
            temperature = payload[2] + payload[3] / 10
        else:
            humidity = ((payload[0] << 8) | payload[1]) / 10
            temperature = (((payload[2] & 0x7F) << 8) | payload[3]) / 10
            if payload[2] & 0x80:
                temperature = -temperature
        return DHTData(
            temperature=temperature + self.temperature_offset,
            humidity=humidity + self.humidity_offset,
        )
