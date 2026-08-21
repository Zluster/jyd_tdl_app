#!/usr/bin/env python3
"""High-level ZW101 API example."""

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
