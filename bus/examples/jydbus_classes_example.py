#!/usr/bin/env python3
"""Example using one Jydbus class per device on a shared UART."""

from __future__ import annotations

import argparse
import pprint
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from jydbus_api import jydbusApi
from jydbus_devices import JydbusBMP390


def main() -> int:
    parser = argparse.ArgumentParser(description="Jydbus class example")
    parser.add_argument("--device", default="/dev/ttyS2")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--number", type=int, default=1)
    args = parser.parse_args()

    with jydbusApi(args.device, args.baud) as api:
        bmp390 = JydbusBMP390(api, args.number)
        pprint.pprint(bmp390.query_value(), sort_dicts=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
