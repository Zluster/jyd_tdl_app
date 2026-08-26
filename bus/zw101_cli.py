#!/usr/bin/env python3
"""Command-line equivalent of zw101_tool."""

from __future__ import annotations

import argparse
import errno
import sys

from jydbus_uart import JydbusUart
from zw101_api import zw101_status_string
from zw101_control import (ZW101_CONTROL_CLEAR_DATABASE, ZW101_CONTROL_DELETE,
                           ZW101_CONTROL_ENROLL, ZW101_CONTROL_MATCH,
                           ZW101_CONTROL_DEFAULT_TIMEOUT_MS, zw101_control_execute)


def number(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 255:
        raise argparse.ArgumentTypeError("value must be 0..255")
    return value


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="Control a cascaded ZW101 node")
    root.add_argument("uart")
    sub = root.add_subparsers(dest="action", required=True)
    for action in ("enroll", "delete"):
        command = sub.add_parser(action)
        command.add_argument("id", type=number)
        command.add_argument("node", type=number, nargs="?", default=1)
    for action in ("match", "clear"):
        command = sub.add_parser(action)
        command.add_argument("node", type=number, nargs="?", default=1)
    return root


def main() -> int:
    args = parser().parse_args()
    operations = {"enroll": ZW101_CONTROL_ENROLL, "match": ZW101_CONTROL_MATCH,
                  "delete": ZW101_CONTROL_DELETE, "clear": ZW101_CONTROL_CLEAR_DATABASE}
    operation = operations[args.action]
    fingerprint_id = getattr(args, "id", 0)
    try:
        with JydbusUart(args.uart, 115200) as uart:
            print(f"ZW101 start: operation={operation} node={args.node} id={fingerprint_id}")
            if operation == ZW101_CONTROL_ENROLL:
                print("Press and fully release the same finger three times.")
            elif operation == ZW101_CONTROL_MATCH:
                print("Place a finger on the sensor.")
            result = zw101_control_execute(uart, args.node, operation, fingerprint_id,
                                           ZW101_CONTROL_DEFAULT_TIMEOUT_MS)
            print(f"ZW101 result: operation={result.operation} "
                  f"status={zw101_status_string(result.status)}({result.status}) "
                  f"module_status=0x{result.module_status:02X} "
                  f"id={result.fingerprint_id} score={result.score} "
                  f"elapsed_ms={result.response_ms}")
            if result.status:
                raise OSError(errno.EIO, "device reported operation failure")
        return 0
    except (OSError, TimeoutError) as exc:
        print(f"ZW101 operation failed: {exc.strerror or exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
