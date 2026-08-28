#!/usr/bin/env python3
"""Linux 端 MFRC522 串口调试工具。"""

from __future__ import annotations

import argparse
import errno
import sys
import time
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from devices import MFRC522Reader
from jydbus_bus import JydBus
from jydbus_uart import (JYDBUS_TYPE_MFRC522, JYDBUS_UART_COMMAND_SCAN,
                         JydbusData)

DEFAULT_DEVICE = "/dev/ttyS2"
DEFAULT_BAUD_RATE = 115200


def parse_integer(text: str) -> int:
    return int(text, 0)


def format_bytes(value: object) -> str:
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).hex(" ").upper()
    return "--"


def print_card(data: JydbusData) -> None:
    if not data.decoded_valid:
        print(f"节点 {data.sensor_number}: 数据无法解析，raw={format_bytes(data.raw)}")
        return
    uid = format_bytes(data.value.get("uid"))
    tag_type = format_bytes(data.value.get("tag_type"))
    present = bool(data.value.get("present"))
    status = int(data.value.get("status", int(present)))
    version = int(data.value.get("version", 0))
    if status == 3 or version in (0x00, 0xFF):
        print(f"节点 {data.sensor_number}: RC522 I2C通信失败, "
              f"VersionReg=0x{version:02X}")
        return
    if status == 2:
        print(f"节点 {data.sensor_number}: 检测到卡，但UID校验失败, "
              f"卡类型={tag_type}, RC522版本=0x{version:02X}")
        return
    state = "检测到卡" if present else "无卡"
    print(f"节点 {data.sensor_number}: {state}, UID={uid}, "
          f"卡类型={tag_type}, RC522版本=0x{version:02X}")


def scan_readers(bus: JydBus) -> list[int]:
    result = bus.run_command(JYDBUS_UART_COMMAND_SCAN)
    if result.status != 0:
        raise OSError(-result.status, "扫描命令发送失败")
    nodes = sorted({step.sensor_number for step in result.steps
                    if step.sensor_type == JYDBUS_TYPE_MFRC522})
    print("发现 MFRC522 节点: " + ", ".join(map(str, nodes))
          if nodes else "未发现 MFRC522 节点")
    return nodes


def query_card(reader: MFRC522Reader) -> JydbusData:
    data = reader.request_data()
    print_card(data)
    return data


def listen_for_cards(reader: MFRC522Reader, interval_ms: int) -> None:
    reader.enable_auto_upload(interval_ms)
    print(f"监听 MFRC522 节点 {reader.sensor_number}，按 Ctrl+C 结束")
    try:
        last_sequence = query_card(reader).sequence
        while True:
            try:
                data = reader.read_cached_data()
            except OSError as exc:
                if exc.errno != errno.ENODATA:
                    raise
            else:
                if data.sequence != last_sequence:
                    last_sequence = data.sequence
                    print_card(data)
            time.sleep(0.02)
    finally:
        reader.disable_auto_upload()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MFRC522 串口调试工具")
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument("--baud", type=parse_integer, default=DEFAULT_BAUD_RATE)
    parser.add_argument("--number", type=parse_integer, choices=range(1, 9),
                        default=1, metavar="1..8")
    parser.add_argument("--interval", type=parse_integer, default=200)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--scan", action="store_true")
    mode.add_argument("--listen", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.interval <= 65535:
        parser.error("--interval 必须在 1..65535 之间")
    return args


def main() -> int:
    args = parse_args()
    try:
        with JydBus(args.device, args.baud) as bus:
            reader = MFRC522Reader(bus, args.number)
            if args.scan:
                scan_readers(bus)
            elif args.listen:
                listen_for_cards(reader, args.interval)
            else:
                query_card(reader)
        return 0
    except KeyboardInterrupt:
        print("\n监听已停止")
        return 0
    except (OSError, TimeoutError) as exc:
        print(f"错误: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
