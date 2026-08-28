#!/usr/bin/env python3
"""Test the public sensor interfaces against connected hardware."""

from __future__ import annotations

import argparse
import pprint
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from devices import (AHT10Sensor, BMP390Sensor, ButtonPB1Sensor,
                     JoystickSensor, KnobSwitchSensor, MAX30102Sensor,
                     MFRC522Reader, PAJ7620U2GestureSensor,
                     PhotoresistorSensor, SensorDevice, SoilMoistureSensor,
                     VL53L0XSensor, WS2812BPanel, WaterLevelSensor,
                     ZSPD4003Sensor, ZW101FingerprintSensor)
from jydbus_bus import JydBus
from jydbus_uart import JYDBUS_TYPE_ZW101, JYDBUS_UART_COMMAND_SCAN
from sensor_ota import SENSOR_OTA_SLOT_AUTO, SensorOta, SensorOtaStatus

SensorTarget = tuple[int, int]


@dataclass
class TestSummary:
    passed: int = 0
    failed: int = 0
    skipped: int = 0

    def pass_test(self, label: str) -> None:
        self.passed += 1
        print(f"[PASS] {label}")

    def fail_test(self, label: str, exc: BaseException) -> None:
        self.failed += 1
        print(f"[FAIL] {label}: {exc}")

    def skip_test(self, label: str, reason: str) -> None:
        self.skipped += 1
        print(f"[SKIP] {label}: {reason}")


READ_METHOD_BY_CLASS: dict[type[SensorDevice], str] = {
    AHT10Sensor: "measure_temperature_humidity",
    BMP390Sensor: "measure_temperature_pressure",
    MAX30102Sensor: "measure_heart_rate_oxygen",
    VL53L0XSensor: "measure_distance",
    MFRC522Reader: "read_card",
    WS2812BPanel: "request_value",
    ButtonPB1Sensor: "read_button",
    JoystickSensor: "read_position",
    PhotoresistorSensor: "measure_light_level",
    WaterLevelSensor: "measure_water_level",
    SoilMoistureSensor: "measure_soil_moisture",
    ZSPD4003Sensor: "measure_heart_rate_oxygen",
    KnobSwitchSensor: "read_position",
    PAJ7620U2GestureSensor: "read_gesture",
}


def parse_integer(text: str) -> int:
    return int(text, 0)


def parse_target(text: str) -> SensorTarget:
    try:
        type_text, number_text = text.split(":", 1)
        sensor_type = int(type_text, 0)
        sensor_number = int(number_text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "target must use TYPE:NUMBER, for example 0x03:1") from exc
    if not 0 <= sensor_type <= 0xFF or not 1 <= sensor_number <= 8:
        raise argparse.ArgumentTypeError(
            "TYPE must be 0..255 and NUMBER must be 1..8")
    return sensor_type, sensor_number


def parse_write(text: str) -> tuple[int, int, int]:
    try:
        type_text, number_text, value_text = text.split(":", 2)
        sensor_type = int(type_text, 0)
        sensor_number = int(number_text, 0)
        value = int(value_text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "write must use TYPE:NUMBER:VALUE") from exc
    if not 0 <= sensor_type <= 0xFF or not 1 <= sensor_number <= 8:
        raise argparse.ArgumentTypeError(
            "TYPE must be 0..255 and NUMBER must be 1..8")
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("VALUE must be 0..0xFFFFFFFF")
    return sensor_type, sensor_number, value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Test Jydbus sensor interfaces on real hardware")
    parser.add_argument("--device", default="/dev/ttyS2")
    parser.add_argument("--baud", type=parse_integer, default=115200)
    parser.add_argument(
        "--sensor", type=parse_target, action="append", default=[],
        metavar="TYPE:NUMBER",
        help="test a specified node; repeat to test multiple nodes")
    parser.add_argument(
        "--auto-upload", action="store_true",
        help="also test enabling, reading and disabling automatic upload")
    parser.add_argument("--auto-upload-seconds", type=float, default=0.3)
    parser.add_argument(
        "--write", type=parse_write, action="append", default=[],
        metavar="TYPE:NUMBER:VALUE",
        help="perform an explicit state-changing generic write")
    parser.add_argument(
        "--ws2812b-node", type=parse_integer, metavar="NUMBER",
        help="set pixel 0 red, display a color frame, then turn the panel off")
    parser.add_argument(
        "--fingerprint-match-node", type=parse_integer, metavar="NUMBER",
        help="perform a ZW101 fingerprint match")
    parser.add_argument("--fingerprint-timeout", type=parse_integer,
                        default=35000, metavar="MS")
    parser.add_argument(
        "--ota-status", type=parse_target, metavar="TYPE:NUMBER",
        help="query OTA A/B status after closing the sensor bus")
    return parser


def scan_targets(bus: JydBus, summary: TestSummary) -> list[SensorTarget]:
    result = bus.run_command(JYDBUS_UART_COMMAND_SCAN)
    if result.status != 0:
        raise OSError(-result.status, "bus scan failed")
    targets = sorted({(step.sensor_type, step.sensor_number)
                      for step in result.steps})
    summary.pass_test(f"bus scan found {len(targets)} node(s)")
    return targets


def test_sensor(bus: JydBus, target: SensorTarget,
                auto_upload: bool, wait_seconds: float,
                summary: TestSummary) -> None:
    sensor_type, sensor_number = target
    label = f"type=0x{sensor_type:02X} number={sensor_number}"
    try:
        sensor = bus.create_device(sensor_type, sensor_number)
        summary.pass_test(f"{label} create {sensor.__class__.__name__}")
    except Exception as exc:
        summary.fail_test(f"{label} create device", exc)
        return

    read_method_name = READ_METHOD_BY_CLASS.get(type(sensor))
    if read_method_name is None:
        if sensor_type == JYDBUS_TYPE_ZW101:
            summary.skip_test(
                f"{label} fingerprint operation",
                "use --fingerprint-match-node to test interactively")
        else:
            summary.skip_test(f"{label} read", "no read method registered")
    else:
        try:
            read_method: Callable[[], dict[str, object]] = getattr(
                sensor, read_method_name)
            value = read_method()
            print(f"       value={pprint.pformat(value, sort_dicts=True)}")
            cached = sensor.read_cached_value()
            if cached != value:
                raise RuntimeError("cached value differs from query result")
            sensor.request_update()
            summary.pass_test(
                f"{label} {read_method_name}, cache and request_update")
        except Exception as exc:
            summary.fail_test(f"{label} {read_method_name}", exc)

    if auto_upload and sensor_type != JYDBUS_TYPE_ZW101:
        try:
            sensor.enable_auto_upload()
            time.sleep(wait_seconds)
            value = sensor.read_cached_value()
            print(f"       auto_upload={pprint.pformat(value, sort_dicts=True)}")
            summary.pass_test(f"{label} automatic upload")
        except Exception as exc:
            summary.fail_test(f"{label} automatic upload", exc)
        finally:
            try:
                sensor.disable_auto_upload()
            except Exception as exc:
                summary.fail_test(f"{label} disable automatic upload", exc)


def test_generic_writes(bus: JydBus,
                        writes: list[tuple[int, int, int]],
                        summary: TestSummary) -> None:
    for sensor_type, sensor_number, value in writes:
        label = (f"write type=0x{sensor_type:02X} number={sensor_number} "
                 f"value=0x{value:08X}")
        try:
            sensor = bus.create_device(sensor_type, sensor_number)
            sensor.write_value(value)
            summary.pass_test(label)
        except Exception as exc:
            summary.fail_test(label, exc)


def test_ws2812b(bus: JydBus, sensor_number: int,
                 summary: TestSummary) -> None:
    label = f"WS2812B number={sensor_number}"
    panel = WS2812BPanel(bus, sensor_number)
    try:
        panel.set_pixel_color(0, 0xFF0000)
        colors = [0x000000] * 128
        colors[0] = 0xFF0000
        colors[127] = 0x0000FF
        panel.display_frame(colors)
        summary.pass_test(f"{label} pixel and frame")
    except Exception as exc:
        summary.fail_test(f"{label} pixel and frame", exc)
    finally:
        try:
            panel.display_frame([0x000000] * 128)
        except Exception as exc:
            summary.fail_test(f"{label} turn off", exc)


def test_fingerprint(bus: JydBus, sensor_number: int, timeout_ms: int,
                     summary: TestSummary) -> None:
    label = f"ZW101 number={sensor_number} match_fingerprint"
    try:
        sensor = ZW101FingerprintSensor(bus, sensor_number)
        sensor.set_operation_timeout(timeout_ms)
        print("Place a finger on the ZW101 sensor.")
        result = sensor.match_fingerprint()
        print(f"       id={result.fingerprint_id} score={result.score}")
        summary.pass_test(label)
    except Exception as exc:
        summary.fail_test(label, exc)


def slot_name(slot: int) -> str:
    return {0: "A", 1: "B", SENSOR_OTA_SLOT_AUTO: "none"}.get(slot,
                                                                 str(slot))


def test_ota_status(device: str, baud: int, target: SensorTarget,
                    summary: TestSummary) -> None:
    sensor_type, sensor_number = target
    label = f"OTA status type=0x{sensor_type:02X} number={sensor_number}"
    try:
        with SensorOta(device, baud) as updater:
            status: SensorOtaStatus = updater.get_status(sensor_type,
                                                         sensor_number)
        print(f"       active={slot_name(status.active_slot)} "
              f"confirmed={slot_name(status.confirmed_slot)} "
              f"pending={slot_name(status.pending_slot)} "
              f"offset={status.next_offset}")
        summary.pass_test(label)
    except Exception as exc:
        summary.fail_test(label, exc)


def validate_args(parser: argparse.ArgumentParser,
                  args: argparse.Namespace) -> None:
    if args.auto_upload_seconds < 0:
        parser.error("--auto-upload-seconds must be non-negative")
    if args.ws2812b_node is not None and not 1 <= args.ws2812b_node <= 8:
        parser.error("--ws2812b-node must be 1..8")
    if (args.fingerprint_match_node is not None
            and not 1 <= args.fingerprint_match_node <= 8):
        parser.error("--fingerprint-match-node must be 1..8")
    if args.fingerprint_timeout <= 0:
        parser.error("--fingerprint-timeout must be positive")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    validate_args(parser, args)
    summary = TestSummary()

    try:
        with JydBus(args.device, args.baud) as bus:
            targets = args.sensor or scan_targets(bus, summary)
            if not targets:
                raise RuntimeError("no sensor nodes found")
            for target in targets:
                test_sensor(bus, target, args.auto_upload,
                            args.auto_upload_seconds, summary)
            test_generic_writes(bus, args.write, summary)
            if args.ws2812b_node is not None:
                test_ws2812b(bus, args.ws2812b_node, summary)
            if args.fingerprint_match_node is not None:
                test_fingerprint(bus, args.fingerprint_match_node,
                                 args.fingerprint_timeout, summary)
    except Exception as exc:
        summary.fail_test("open or use sensor bus", exc)

    if args.ota_status is not None:
        test_ota_status(args.device, args.baud, args.ota_status, summary)

    print(f"\nSummary: passed={summary.passed} failed={summary.failed} "
          f"skipped={summary.skipped}")
    return 1 if summary.failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
