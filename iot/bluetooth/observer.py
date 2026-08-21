"""Bluetooth LE beacon scanning and Dara-owned parsed beacon records."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any, cast
from uuid import UUID

from ._codec import (
    has_service,
    process_eddystone,
    process_ibeacon,
)
from .errors import DeviceNotFoundError
from .adapter import Adapter
from .device import Device


_EDDYSTONE_UUID = "0000feaa-0000-1000-8000-00805f9b34fb"


class EddystoneURL:
    """Parsed Eddystone URL frame."""

    url: str
    """Decoded advertised URL."""
    tx_pwr: int
    """Calibrated transmit power in dBm."""
    rssi: int
    """Received signal strength in dBm."""

    def __init__(self, url: str, tx_pwr: int, rssi: int) -> None:
        """Initialize a parsed Eddystone URL frame."""
        self.url = url
        self.tx_pwr = tx_pwr
        self.rssi = rssi


class EddystoneUID:
    """Parsed Eddystone UID frame."""

    namespace: int
    """Ten-byte Eddystone namespace identifier."""
    instance: int
    """Six-byte Eddystone instance identifier."""
    tx_pwr: int
    """Calibrated transmit power in dBm."""
    rssi: int
    """Received signal strength in dBm."""

    def __init__(
        self, namespace: int, instance: int, tx_pwr: int, rssi: int
    ) -> None:
        """Initialize a parsed Eddystone UID frame."""
        self.namespace = namespace
        self.instance = instance
        self.tx_pwr = tx_pwr
        self.rssi = rssi


class EddystoneTLM:
    """Parsed Eddystone telemetry frame."""

    version: int
    """Telemetry frame version."""
    battery: int
    """Battery voltage in millivolts."""
    temperature: float
    """Beacon temperature in degrees Celsius."""
    count: int
    """Advertisement frame count since power-on or reboot."""
    uptime: float
    """Elapsed beacon uptime in seconds."""
    tx_pwr: int
    """Frame transmit-power field retained for API consistency."""
    rssi: int
    """Received signal strength in dBm."""

    def __init__(
        self,
        version: int,
        battery: int,
        temperature: float,
        count: int,
        uptime: float,
        tx_pwr: int,
        rssi: int,
    ) -> None:
        """Initialize a parsed Eddystone telemetry frame."""
        self.version = version
        self.battery = battery
        self.temperature = temperature
        self.count = count
        self.uptime = uptime
        self.tx_pwr = tx_pwr
        self.rssi = rssi


class IBeacon:
    """Parsed Apple iBeacon manufacturer frame."""

    uuid: UUID
    """Beacon proximity UUID."""
    major: int
    """Beacon group identifier."""
    minor: int
    """Beacon unit identifier."""
    tx_pwr: int
    """Calibrated transmit power in dBm."""
    rssi: int
    """Received signal strength in dBm."""

    def __init__(
        self, uuid: UUID, major: int, minor: int, tx_pwr: int, rssi: int
    ) -> None:
        """Initialize a parsed iBeacon frame."""
        self.uuid = uuid
        self.major = major
        self.minor = minor
        self.tx_pwr = tx_pwr
        self.rssi = rssi


class AltBeacon:
    """Parsed AltBeacon manufacturer frame."""

    uuid: UUID
    """Beacon identifier represented as a UUID."""
    major: int
    """First two-byte AltBeacon data identifier."""
    minor: int
    """Second two-byte AltBeacon data identifier."""
    tx_pwr: int
    """Calibrated transmit power in dBm."""
    rssi: int
    """Received signal strength in dBm."""

    def __init__(
        self, uuid: UUID, major: int, minor: int, tx_pwr: int, rssi: int
    ) -> None:
        """Initialize a parsed AltBeacon frame."""
        self.uuid = uuid
        self.major = major
        self.minor = minor
        self.tx_pwr = tx_pwr
        self.rssi = rssi


class Scanner:
    """Scan for common Bluetooth LE beacon formats on one adapter."""

    adapter: Adapter
    """Local Bluetooth adapter used for beacon discovery."""
    on_eddystone_url: Callable[[EddystoneURL], Any] | None
    """Callback invoked for parsed Eddystone URL frames."""
    on_eddystone_uid: Callable[[EddystoneUID], Any] | None
    """Callback invoked for parsed Eddystone UID frames."""
    on_eddystone_tlm: Callable[[EddystoneTLM], Any] | None
    """Callback invoked for parsed Eddystone telemetry frames."""
    on_ibeacon: Callable[[IBeacon], Any] | None
    """Callback invoked for parsed iBeacon frames."""
    on_altbeacon: Callable[[AltBeacon], Any] | None
    """Callback invoked for parsed AltBeacon frames."""

    def __init__(self, adapter_address: str | None = None) -> None:
        """Create an inactive scanner on a selected local adapter."""
        self.adapter = Adapter(adapter_address)
        self.on_eddystone_url = None
        self.on_eddystone_uid = None
        self.on_eddystone_tlm = None
        self.on_ibeacon = None
        self.on_altbeacon = None
        self._remove_paths: set[str] = set()
        self._scanning = False
        self._previous_device_callback: Callable[..., Any] | None = None

    @property
    def scanning(self) -> bool:
        """Return whether beacon discovery is active."""
        return self._scanning

    def start_beacon_scan(
        self,
        *,
        on_eddystone_url: Callable[[EddystoneURL], Any] | None = None,
        on_eddystone_uid: Callable[[EddystoneUID], Any] | None = None,
        on_eddystone_tlm: Callable[[EddystoneTLM], Any] | None = None,
        on_ibeacon: Callable[[IBeacon], Any] | None = None,
        on_altbeacon: Callable[[AltBeacon], Any] | None = None,
    ) -> None:
        """Start nonblocking beacon discovery with per-format callbacks."""
        callbacks = (
            on_eddystone_url,
            on_eddystone_uid,
            on_eddystone_tlm,
            on_ibeacon,
            on_altbeacon,
        )
        (
            self.on_eddystone_url,
            self.on_eddystone_uid,
            self.on_eddystone_tlm,
            self.on_ibeacon,
            self.on_altbeacon,
        ) = callbacks
        self._previous_device_callback = self.adapter.on_device_found
        self.adapter.on_device_found = self._on_device_found
        if not self.adapter.powered:
            self.adapter.powered = True
        self.adapter.show_duplicates()
        self.adapter.start_discovery()
        self._scanning = True

    def stop_scan(self) -> None:
        """Stop beacon discovery and restore the prior device callback."""
        if not self._scanning:
            return
        self.adapter.stop_discovery()
        self.adapter.on_device_found = self._previous_device_callback
        self._previous_device_callback = None
        self._scanning = False

    def clean_beacons(self) -> None:
        """Remove recognized devices so BlueZ reports their next advertisement."""
        for path in tuple(self._remove_paths):
            try:
                self.adapter.remove_device(path)
            except DeviceNotFoundError:
                pass
            self._remove_paths.discard(path)

    def _on_device_found(self, device: Device) -> None:
        """Parse recognized advertisement data and dispatch one beacon record."""
        rssi = device.rssi
        if rssi is None:
            return
        recognized = False
        try:
            service_data = device.service_data
            if service_data and has_service(0xFEAA, service_data):
                payload = next(
                    (
                        value
                        for uuid, value in service_data.items()
                        if uuid.casefold() == _EDDYSTONE_UUID
                    ),
                    None,
                )
                if payload is not None:
                    parsed = process_eddystone(payload, rssi)
                    if parsed is not None:
                        recognized = True
                        self._dispatch_eddystone(parsed)

            if not recognized:
                for company_id, payload in device.manufacturer_data.items():
                    if (
                        company_id == 0x004C
                        and len(payload) >= 2
                        and payload[:2] == b"\x02\x15"
                    ):
                        parsed = process_ibeacon(payload, rssi)
                        if parsed is not None:
                            recognized = True
                            self._dispatch_ibeacon(parsed, alt=False)
                            break
                    if len(payload) >= 2 and payload[:2] == b"\xbe\xac":
                        parsed = process_ibeacon(payload, rssi, "AltBeacon")
                        if parsed is not None:
                            recognized = True
                            self._dispatch_ibeacon(parsed, alt=True)
                            break
        finally:
            if recognized:
                self._remove_paths.add(device._path)
                self.clean_beacons()

    def _dispatch_eddystone(self, parsed: dict[str, Any]) -> bool:
        """Create and dispatch the Eddystone record selected by ``kind``."""
        kind = parsed.get("kind")
        if kind == "url":
            value = EddystoneURL(
                url=str(parsed["url"]),
                tx_pwr=int(parsed["tx_pwr"]),
                rssi=int(parsed["rssi"]),
            )
            self._invoke(self.on_eddystone_url, value)
            return True
        if kind == "uid":
            value = EddystoneUID(
                namespace=int(parsed["namespace"]),
                instance=int(parsed["instance"]),
                tx_pwr=int(parsed["tx_pwr"]),
                rssi=int(parsed["rssi"]),
            )
            self._invoke(self.on_eddystone_uid, value)
            return True
        if kind == "tlm":
            value = EddystoneTLM(
                version=int(parsed["version"]),
                battery=int(parsed["battery"]),
                temperature=float(parsed["temperature"]),
                count=int(parsed["count"]),
                uptime=float(parsed["uptime"]),
                tx_pwr=int(parsed["tx_pwr"]),
                rssi=int(parsed["rssi"]),
            )
            self._invoke(self.on_eddystone_tlm, value)
            return True
        return False

    def _dispatch_ibeacon(self, parsed: dict[str, Any], *, alt: bool) -> None:
        """Create and dispatch an iBeacon or AltBeacon Dara record."""
        common = {
            "uuid": cast(UUID, parsed["uuid"]),
            "major": int(parsed["major"]),
            "minor": int(parsed["minor"]),
            "tx_pwr": int(parsed["tx_pwr"]),
            "rssi": int(parsed["rssi"]),
        }
        if alt:
            self._invoke(self.on_altbeacon, AltBeacon(**common))
        else:
            self._invoke(self.on_ibeacon, IBeacon(**common))

    @staticmethod
    def _invoke(callback: Callable[[Any], Any] | None, value: object) -> None:
        """Invoke a configured scanner callback at its declared arity."""
        if callback is not None:
            callback(value)


def scan_eddystone(
    on_data: Callable[[EddystoneURL], Any] | None = None,
    *,
    adapter_address: str | None = None,
) -> Scanner:
    """Start a nonblocking Eddystone URL scan and return its scanner."""
    scanner = Scanner(adapter_address)
    scanner.start_beacon_scan(on_eddystone_url=on_data)
    return scanner


__all__ = [
    "AltBeacon",
    "EddystoneTLM",
    "EddystoneUID",
    "EddystoneURL",
    "IBeacon",
    "Scanner",
    "scan_eddystone",
]
