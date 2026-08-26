#!/usr/bin/env python3
"""High-level ZW101 API example."""

import sys
from pathlib import Path

# Allow `python3 examples/zw101_api_example.py` from the project directory.
PROJECT_DIR = Path(__file__).resolve().parent.parent
if str(PROJECT_DIR) not in sys.path:
    sys.path.insert(0, str(PROJECT_DIR))

from zw101_api import Zw101Device


def main() -> None:
    with Zw101Device("/dev/ttyS2", 1) as fingerprint:
        fingerprint.enroll(2)
        result = fingerprint.match()
        print(f"Fingerprint matched: id={result.fingerprint_id} score={result.score}")
        # fingerprint.delete(2)
        # fingerprint.clear()


if __name__ == "__main__":
    main()
