#!/usr/bin/env python3
"""Hardware scan debug script."""

import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from examples.api_call_example import getScanData, initialize


def main() -> int:
    api = initialize("/dev/ttyS2", 115200)
    try:
        print("getScanData() result:")
        print(getScanData(api))
        return 0
    finally:
        api.close()


if __name__ == "__main__":
    raise SystemExit(main())
