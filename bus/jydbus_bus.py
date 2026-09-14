"""High-level interface for one shared Jydbus UART connection."""

from __future__ import annotations

import errno
from typing import TYPE_CHECKING

if __package__:
    from .jydbus_uart import (JYDBUS_UART_COMMAND_SCAN, JydbusData,
                              JydbusUart, JydbusUartCommandResult,
                              jydbus_name)
else:
    from jydbus_uart import (JYDBUS_UART_COMMAND_SCAN, JydbusData,
                             JydbusUart, JydbusUartCommandResult,
                             jydbus_name)

if TYPE_CHECKING:
    if __package__:
        from .devices import SensorDevice
    else:
        from devices import SensorDevice


class JydBus:
    """Own one UART connection and its receive thread."""

    def __init__(self, device: str, baud_rate: int = 115200) -> None:
        self.uart = JydbusUart(device, baud_rate)

    def close(self) -> None:
        self.uart.close()

    def request_sensor(self, sensor_type: int, sensor_number: int) -> int:
        """Send a sensor query without waiting for its response."""
        return self.uart.request_sensor(sensor_type, sensor_number)

    def read_cached(self, sensor_type: int, sensor_number: int) -> JydbusData:
        """Return the latest cached response for one sensor."""
        return self.uart.read_cached(sensor_type, sensor_number)

    def read_all_cached(self) -> list[JydbusData]:
        return self.uart.read_all_cached()

    def write_sensor_value(self, sensor_type: int, sensor_number: int,
                           value: int) -> int:
        return self.uart.write_sensor_value(sensor_type, sensor_number, value)

    def configure_auto_upload(self, sensor_type: int, sensor_number: int,
                              enabled: bool, interval_ms: int) -> int:
        return self.uart.configure_auto_upload(sensor_type, sensor_number,
                                               enabled, interval_ms)

    def scan(self) -> list[tuple[str, int]]:
        """Return all discovered nodes as ``(sensor_name, sensor_number)``."""
        result = self.uart.execute_command(JYDBUS_UART_COMMAND_SCAN)
        if result.status != 0:
            code = -result.status if result.status < 0 else result.status
            raise OSError(code or errno.EIO, "bus scan failed")
        return [(jydbus_name(step.sensor_type), step.sensor_number)
                for step in result.steps]

    def run_command(self, command: int, sensor_type: int = 0,
                    sensor_number: int = 0,
                    value: int = 0) -> JydbusUartCommandResult:
        return self.uart.execute_command(command, sensor_type,
                                         sensor_number, value)

    def create_device(self, sensor_type: int,
                      sensor_number: int = 1) -> SensorDevice:
        if __package__:
            from .devices import create_device
        else:
            from devices import create_device

        return create_device(self, sensor_type, sensor_number)

    def __enter__(self) -> "JydBus":
        return self

    def __exit__(self, exc_type: object, exc: object,
                 traceback: object) -> None:
        self.close()
