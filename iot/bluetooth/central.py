"""Convenience wrapper for using Bluetooth Low Energy in the Central role."""

from __future__ import annotations

import asyncio

from . import _runtime
from .errors import BluetoothError, BluetoothTimeoutError
from .adapter import Adapter
from .device import Device
from .gatt import RemoteCharacteristic


_METHOD_TIMEOUT_MARGIN = 5.0


class Central:
    """Connect to a remote device and manage selected GATT characteristics."""

    adapter: Adapter
    """Local adapter used for the Central role."""
    device: Device
    """Remote device managed by this Central."""

    @staticmethod
    def available(adapter_address: str | None = None) -> list[Device]:
        """Return remote devices discovered by an optional local adapter."""
        return Device.available(adapter_address)

    def __init__(self, device_addr: str, adapter_addr: str | None = None) -> None:
        """Select a remote device and ensure its local adapter is powered."""
        self.adapter = Adapter(adapter_addr)
        if not self.adapter.powered:
            self.adapter.powered = True
        self.device = Device(self.adapter.address, device_addr)
        self._characteristics: list[RemoteCharacteristic] = []

    def add_characteristic(
        self,
        service_uuid: str,
        characteristic_uuid: str,
    ) -> RemoteCharacteristic:
        """Track a remote characteristic identified by its service and UUID."""
        characteristic = RemoteCharacteristic(
            self.adapter.address,
            self.device.address,
            service_uuid,
            characteristic_uuid,
        )
        self._characteristics.append(characteristic)
        return characteristic

    def load_gatt(self) -> None:
        """Resolve every characteristic currently tracked by this Central."""
        for characteristic in self._characteristics:
            characteristic.resolve_gatt()

    @property
    def services_available(self) -> list[str]:
        """Return GATT service UUIDs currently exposed by the remote device."""
        return self.device.services_available

    @property
    def services_resolved(self) -> bool:
        """Return whether BlueZ has resolved the remote GATT services."""
        return self.device.services_resolved

    @property
    def connected(self) -> bool:
        """Return whether the remote device is connected."""
        return self.device.connected

    def connect(self, profile: str | None = None, timeout: float = 35.0) -> None:
        """Connect, wait for service discovery, and resolve tracked GATT objects."""
        self.device.connect(profile, float(timeout))
        try:
            _runtime.run(
                self._wait_for_services(float(timeout)),
                timeout=float(timeout) + _METHOD_TIMEOUT_MARGIN,
            )
        except BluetoothTimeoutError:
            try:
                self.device.disconnect()
            except BluetoothError:
                pass
            raise
        self.load_gatt()

    async def _wait_for_services(self, timeout: float) -> None:
        """Poll BlueZ asynchronously until services resolve or time expires."""

        async def wait() -> None:
            """Poll the ServicesResolved property without blocking the loop."""
            while not await self.device._get_async("ServicesResolved", False):
                await asyncio.sleep(0.1)

        try:
            await asyncio.wait_for(wait(), timeout)
        except asyncio.TimeoutError as error:
            raise BluetoothTimeoutError(
                f"GATT service resolution timed out after {timeout:g} seconds"
            ) from error

    def disconnect(self) -> None:
        """Disconnect the remote device."""
        self.device.disconnect()


__all__ = ["Central"]
