"""Test QMI8658 read functions with an execution-time decorator."""

from typing import Any

from dara.device.qmi8658 import QMI8658
from dara.peripheral.i2c import I2C

from timer import measure_time


def _show_result(name: str, result: Any) -> None:
    """Print a driver result, expanding simple data objects when possible."""
    value = getattr(result, "__dict__", result)
    print(f"{name} result: {value}")


@measure_time
def test_read_raw(sensor: QMI8658) -> Any:
    """Read and return unconverted QMI8658 sensor values."""
    return sensor.read_raw()


@measure_time
def test_read_imu(sensor: QMI8658) -> Any:
    """Read and return acceleration and gyroscope values."""
    return sensor.read_imu()


@measure_time
def test_read_temperature(sensor: QMI8658) -> Any:
    """Read and return the QMI8658 temperature."""
    return sensor.read_temperature()


@measure_time
def test_read_all(sensor: QMI8658) -> Any:
    """Read and return all QMI8658 values."""
    return sensor.read_all()


def main() -> None:
    """Open the sensor, run all decorated checks, and close resources."""
    with I2C(0) as bus, QMI8658(bus) as sensor:
        checks = (
            ("read_raw", test_read_raw),
            ("read_imu", test_read_imu),
            ("read_temperature", test_read_temperature),
            ("read_all", test_read_all),
        )
        for name, check in checks:
            _show_result(name, check(sensor))


if __name__ == "__main__":
    main()
