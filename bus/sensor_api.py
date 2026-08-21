"""Simple high-level API corresponding to sensor_api.c."""

from __future__ import annotations

from sensor_uart import SensorData, SensorUart, SensorUartCommandResult


class SensorApi:
    def __init__(self, device: str, baud_rate: int = 115200) -> None:
        self.uart = SensorUart(device, baud_rate)

    def close(self) -> None:
        self.uart.close()

    def query(self, sensor_type: int, sensor_number: int) -> int:
        return self.uart.query(sensor_type, sensor_number)

    def read(self, sensor_type: int, sensor_number: int) -> SensorData:
        return self.uart.read(sensor_type, sensor_number)

    def write(self, sensor_type: int, sensor_number: int, value: int) -> int:
        return self.uart.set_value(sensor_type, sensor_number, value)

    def set_auto_upload(self, sensor_type: int, sensor_number: int,
                        enabled: bool, interval_ms: int) -> int:
        return self.uart.set_auto_upload(sensor_type, sensor_number,
                                         enabled, interval_ms)

    def command(self, command: int, sensor_type: int = 0,
                sensor_number: int = 0, value: int = 0) -> SensorUartCommandResult:
        return self.uart.execute_command(command, sensor_type, sensor_number, value)

    def __enter__(self) -> "SensorApi":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()


def sensor_api_open(device: str, baud_rate: int = 115200) -> SensorApi:
    return SensorApi(device, baud_rate)


def sensor_api_close(api: SensorApi) -> None:
    api.close()
