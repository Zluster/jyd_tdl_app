"""Private decoders for Bluetooth beacon payloads."""

from __future__ import annotations

import uuid
from collections.abc import Iterable, Mapping
from typing import Any


_URL_PREFIXES = ("http://www.", "https://www.", "http://", "https://")
_URL_SUFFIXES = (
    ".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/",
    ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov",
)


def _decode_url(data: bytes) -> str:
    """Decode an Eddystone URL frame."""
    url = _URL_PREFIXES[data[2]]
    return url + "".join(
        _URL_SUFFIXES[value] if value < len(_URL_SUFFIXES) else chr(value)
        for value in data[3:]
    )


def process_eddystone(data: bytes | Iterable[int], rssi: int) -> dict[str, Any] | None:
    """Parse an Eddystone service-data payload."""
    data = bytes(data)
    if len(data) < 2:
        return None
    frame_type = data[0]
    tx_power = int.from_bytes(data[1:2], "big", signed=True)
    if frame_type == 0x00 and len(data) >= 18:
        return {
            "kind": "uid",
            "namespace": int.from_bytes(data[2:12], "big"),
            "instance": int.from_bytes(data[12:18], "big"),
            "tx_pwr": tx_power,
            "rssi": rssi,
        }
    if frame_type == 0x10 and len(data) >= 3:
        try:
            url = _decode_url(data)
        except (IndexError, ValueError):
            return None
        return {"kind": "url", "url": url, "tx_pwr": tx_power, "rssi": rssi}
    if frame_type == 0x20 and len(data) >= 14 and data[1] == 0:
        return {
            "kind": "tlm",
            "version": data[1],
            "battery": int.from_bytes(data[2:4], "big"),
            "temperature": int.from_bytes(data[4:6], "big", signed=True) / 256,
            "count": int.from_bytes(data[6:10], "big"),
            "uptime": int.from_bytes(data[10:14], "big") / 10,
            "tx_pwr": tx_power,
            "rssi": rssi,
        }
    return None


def process_ibeacon(
    data: bytes | Iterable[int], rssi: int, beacon_type: str = "iBeacon"
) -> dict[str, Any] | None:
    """Parse an iBeacon or AltBeacon manufacturer payload."""
    data = bytes(data)
    if len(data) < 23:
        return None
    return {
        "kind": beacon_type.casefold(),
        "uuid": uuid.UUID(bytes=data[2:18]),
        "major": int.from_bytes(data[18:20], "big"),
        "minor": int.from_bytes(data[20:22], "big"),
        "tx_pwr": int.from_bytes(data[22:23], "big", signed=True),
        "rssi": rssi,
    }


def has_service(uuid_16: int, service_data: Mapping[Any, Any]) -> bool:
    """Return whether service data contains a Bluetooth SIG 16-bit UUID."""
    expected = f"0000{uuid_16:04x}-0000-1000-8000-00805f9b34fb"
    return any(str(key).casefold() == expected for key in service_data)
