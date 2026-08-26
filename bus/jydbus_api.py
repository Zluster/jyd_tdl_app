"""High-level Jydbus API for one shared UART connection."""

from __future__ import annotations

from typing import TYPE_CHECKING

from jydbus_uart import JydbusData, JydbusUart, JydbusUartCommandResult

if TYPE_CHECKING:
    from jydbus_devices import JydbusDevice


class JydbusApi:
    def __init__(self, device: str, baud_rate: int = 115200) -> None:
        self.uart = JydbusUart(device, baud_rate)

    def close(self) -> None:
        self.uart.close()

    def query(self, sensor_type: int, sensor_number: int) -> int:
        return self.uart.query(sensor_type, sensor_number)

    def read(self, sensor_type: int, sensor_number: int) -> JydbusData:
        return self.uart.read(sensor_type, sensor_number)

    def write(self, sensor_type: int, sensor_number: int, value: int) -> int:
        return self.uart.set_value(sensor_type, sensor_number, value)

    def set_ws2812b_pixel(self, led_index: int, color: int,
                          sensor_number: int = 1) -> int:
        return self.uart.set_ws2812b_pixel(sensor_number, led_index, color)

    def set_ws2812b_frame(self, colors: object, sensor_number: int = 1) -> int:
        return self.uart.set_ws2812b_frame(sensor_number, colors)

    def set_auto_upload(self, sensor_type: int, sensor_number: int,
                        enabled: bool, interval_ms: int) -> int:
        return self.uart.set_auto_upload(sensor_type, sensor_number,
                                         enabled, interval_ms)

    def command(self, command: int, sensor_type: int = 0,
                sensor_number: int = 0, value: int = 0) -> JydbusUartCommandResult:
        return self.uart.execute_command(command, sensor_type, sensor_number, value)

    def jydbus(self, sensor_type: int, sensor_number: int = 1) -> JydbusDevice:
        """Create a Jydbus device object sharing this API's UART."""
        from jydbus_devices import create_jydbus

        return create_jydbus(self, sensor_type, sensor_number)

    def sensor(self, sensor_type: int, sensor_number: int = 1) -> JydbusDevice:
        """Compatibility alias for jydbus()."""
        return self.jydbus(sensor_type, sensor_number)

    def __enter__(self) -> "JydbusApi":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()


def jydbus_api_open(device: str, baud_rate: int = 115200) -> JydbusApi:
    return JydbusApi(device, baud_rate)


def jydbus_api_close(api: JydbusApi) -> None:
    api.close()


def jydbus_api_set_ws2812b_pixel(api: JydbusApi, led_index: int, color: int,
                                sensor_number: int = 1) -> int:
    return api.set_ws2812b_pixel(led_index, color, sensor_number)


def jydbus_api_set_ws2812b_frame(api: JydbusApi, colors: object,
                                sensor_number: int = 1) -> int:
    return api.set_ws2812b_frame(colors, sensor_number)


# Exact spelling requested by the existing integration.
jydbusApi = JydbusApi
