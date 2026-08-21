#!/usr/bin/env python3
"""Interactive equivalent of dual_uart_example (the C build uses one UART)."""

from __future__ import annotations

import argparse
import errno
import select
import sys
import time

from sensor_uart import *  # The example intentionally exposes protocol constants.


def print_data(data: SensorData) -> None:
    frame = (bytes((0x55, data.raw_length, data.frame_type, data.sensor_type,
                    data.sensor_number)) + data.raw
             + data.received_crc.to_bytes(2, "little") + bytes((data.frame_tail,)))
    print("[UART1] RX frame:", frame.hex(" ").upper())
    print(f"Sensor: {sensor_name(data.sensor_type)} type=0x{data.sensor_type:02X} "
          f"number={data.sensor_number}")
    if data.decoded_valid and data.sensor_type == SENSOR_TYPE_PAJ7620U2:
        value = data.value
        print(f"Data: gesture={paj7620_gesture_name(value['gesture'])}"
              f"({value['gesture']}) flags_43=0x{value['gesture_flags']:02X} "
              f"flags_44=0x{value['wave_flags']:02X} "
              f"status={paj7620_status_name(value['status'])}({value['status']})")
    else:
        print("Data:", data.value if data.decoded_valid else data.raw.hex(" ").upper())
    print(f"Time (ms): {data.updated_monotonic_ms}")


def print_result(result: SensorUartCommandResult) -> None:
    if result.command == SENSOR_UART_COMMAND_SCAN:
        print(f"[UART1 SCAN_OK] found={len(result.steps)}")
        for step in result.steps:
            print(f"  sensor={sensor_name(step.sensor_type)} "
                  f"type=0x{step.sensor_type:02X} number={step.sensor_number}")
    elif result.command in (SENSOR_UART_COMMAND_QUERY_ALL, SENSOR_UART_COMMAND_QUERY_SENSOR):
        for step in result.steps:
            state = "QUERY_OK" if step.response_received else "QUERY_TIMEOUT"
            print(f"[{state}] sensor={sensor_name(step.sensor_type)} "
                  f"type=0x{step.sensor_type:02X} number={step.sensor_number} "
                  f"rtt_us={step.response_us}")
        if result.data_valid and result.data is not None:
            print_data(result.data)
    elif result.status:
        print(f"Command failed: {errno.errorcode.get(-result.status, result.status)}")
    elif result.data_valid and result.data is not None:
        print_data(result.data)
    else:
        print("Command sent successfully.")


def parse_command(line: str) -> tuple[int, int, int, int]:
    fields = line.split()
    if not fields:
        raise ValueError
    values = [int(field, 0) for field in fields]
    command = values[0]
    required = 3 if command in (6, 8) else 4 if command == 7 else 1
    if command not in range(1, 9) or len(values) != required:
        raise ValueError
    sensor_type = values[1] if required >= 3 else 0
    sensor_number = values[2] if required >= 3 else 0
    value = values[3] if required == 4 else 0
    return command, sensor_type, sensor_number, value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="/dev/ttyS2")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    menu = ("Commands: 1 scan, 2 query all, 3 enable auto upload "
            "(PAJ7620U2 100 ms, others 1000 ms), "
            "4 disable auto upload, 5 WS2812B red,\n"
            "  6 <type> <number> read, 7 <type> <number> <value> write, "
            "8 <type> <number> query, q quit")
    try:
        with SensorUart(args.device, args.baud) as uart:
            print(f"Listening on {args.device} at {args.baud} 8N1.")
            print(menu)
            sequences: dict[tuple[int, int], int] = {}
            print("command> ", end="", flush=True)
            while True:
                readable, _, _ = select.select([sys.stdin], [], [], 0.1)
                for data in uart.read_all():
                    key = (data.sensor_type, data.sensor_number)
                    if sequences.get(key) != data.sequence:
                        sequences[key] = data.sequence
                        print_data(data)
                        print("command> ", end="", flush=True)
                if not readable:
                    continue
                line = sys.stdin.readline()
                if not line or line[:1].lower() == "q":
                    break
                try:
                    print_result(uart.execute_command(*parse_command(line)))
                except (ValueError, OSError):
                    print("Invalid command. Enter 1-5, '6 type number', "
                          "'7 type number value', '8 type number', or q.")
                print("command> ", end="", flush=True)
        return 0
    except OSError as exc:
        print(f"open {args.device} failed: {exc.strerror or exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
