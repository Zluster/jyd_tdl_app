"""Remote Bluetooth device access through BlueZ."""

from __future__ import annotations

import asyncio
from collections.abc import Mapping
from typing import Any, cast

from dbus_fast.errors import DBusError

from . import _dbus, _runtime
from ._constants import (
    DBUS_OM_IFACE,
    DBUS_PROP_IFACE,
    DEVICE_INTERFACE,
    GATT_SERVICE_IFACE,
)
from .errors import BluetoothTimeoutError, DeviceNotFoundError


_METHOD_TIMEOUT_MARGIN = 5.0


class Device:
    """Manage one remote Bluetooth device exposed by BlueZ."""

    @classmethod
    def available(cls, adapter_address: str | None = None) -> list[Device]:
        """Return discovered devices, optionally limited to one adapter."""
        return _runtime.run(cls._available_async(adapter_address))

    @classmethod
    async def _available_async(
        cls, adapter_address: str | None
    ) -> list[Device]:
        """Initialize wrappers for the currently managed remote devices."""
        bus = await _runtime.get_bus()
        root = await _dbus.get_proxy(bus, "/")
        object_manager = root.get_interface(DBUS_OM_IFACE)
        managed = await _dbus.get_managed_objects(object_manager)
        devices: list[Device] = []
        for path, interfaces in managed.items():
            properties = interfaces.get(DEVICE_INTERFACE)
            if properties is None:
                continue
            adapter = _dbus.adapter_address_from_path(path, managed)
            address = properties.get("Address") or _dbus.address_from_path(path)
            if not isinstance(address, str) or not adapter:
                continue
            if (
                adapter_address is None
                or adapter.casefold() == adapter_address.casefold()
            ):
                proxy = await _dbus.get_proxy(bus, path)
                devices.append(
                    cls._resolved(
                        adapter,
                        address,
                        path,
                        bus,
                        object_manager,
                        proxy.get_interface(DEVICE_INTERFACE),
                        proxy.get_interface(DBUS_PROP_IFACE),
                    )
                )
        return devices

    @classmethod
    def _resolved(
        cls,
        adapter: str,
        address: str,
        path: str,
        bus: Any,
        object_manager: Any,
        manager: Any,
        properties: Any,
    ) -> Device:
        """Construct a wrapper around a path resolved from one object snapshot."""
        instance = cls.__new__(cls)
        instance._adapter_address = adapter
        instance._address = address
        instance._path = path
        instance._bus = bus
        instance._object_manager = object_manager
        instance._manager = manager
        instance._properties = properties
        return instance

    def __init__(self, adapter_addr: str, device_addr: str) -> None:
        """Resolve ``device_addr`` below the local ``adapter_addr``."""
        (
            self._path,
            self._adapter_address,
            self._address,
            self._bus,
            self._object_manager,
            self._manager,
            self._properties,
        ) = _runtime.run(self._initialize(adapter_addr, device_addr))

    @staticmethod
    async def _initialize(
        adapter_addr: str, device_addr: str
    ) -> tuple[str, str, str, Any, Any, Any, Any]:
        """Resolve a remote device and initialize its D-Bus interfaces."""
        bus = await _runtime.get_bus()
        root = await _dbus.get_proxy(bus, "/")
        object_manager = root.get_interface(DBUS_OM_IFACE)
        managed = await _dbus.get_managed_objects(object_manager)
        path = _dbus.find_object_path(
            managed,
            adapter=adapter_addr,
            device=device_addr,
        )
        if path is None:
            raise DeviceNotFoundError(
                f"Bluetooth device {device_addr} was not found on {adapter_addr}"
            )
        adapter = _dbus.adapter_address_from_path(path, managed) or adapter_addr
        properties = managed[path][DEVICE_INTERFACE]
        address = properties.get("Address") or _dbus.address_from_path(path)
        if not isinstance(address, str) or not address:
            raise DeviceNotFoundError("Bluetooth device has no usable address")
        proxy = await _dbus.get_proxy(bus, path)
        return (
            path,
            adapter,
            address,
            bus,
            object_manager,
            proxy.get_interface(DEVICE_INTERFACE),
            proxy.get_interface(DBUS_PROP_IFACE),
        )

    def _get(self, name: str, default: Any = None) -> Any:
        """Read one remote-device property synchronously."""
        return _runtime.run(self._get_async(name, default))

    async def _get_async(self, name: str, default: Any) -> Any:
        """Read one remote-device property on the runtime loop."""
        return await _dbus.get_prop(self._properties, DEVICE_INTERFACE, name, default)

    def _set(self, name: str, value: object, signature: str) -> None:
        """Set one remote-device property synchronously."""
        _runtime.run(self._set_async(name, value, signature))

    async def _set_async(self, name: str, value: object, signature: str) -> None:
        """Set one remote-device property on the runtime loop."""
        await _dbus.set_prop(
            self._properties, DEVICE_INTERFACE, name, value, signature
        )

    @property
    def address(self) -> str:
        """Return the remote-device address."""
        return self._address

    @property
    def name(self) -> str | None:
        """Return the remote system name when known."""
        value = self._get("Name")
        return value if isinstance(value, str) else None

    @property
    def icon(self) -> str | None:
        """Return the freedesktop icon name proposed by BlueZ."""
        value = self._get("Icon")
        return value if isinstance(value, str) else None

    @property
    def bt_class(self) -> int | None:
        """Return the Bluetooth class of the remote device when known."""
        value = self._get("Class")
        return int(value) if isinstance(value, int) else None

    @property
    def appearance(self) -> int | None:
        """Return the Generic Access Profile appearance value when known."""
        value = self._get("Appearance")
        return int(value) if isinstance(value, int) else None

    @property
    def uuids(self) -> list[str]:
        """Return advertised or resolved remote service UUIDs."""
        value = self._get("UUIDs", [])
        return [str(item) for item in value] if isinstance(value, list) else []

    @property
    def paired(self) -> bool:
        """Return whether the remote device is paired."""
        return bool(self._get("Paired", False))

    @property
    def connected(self) -> bool:
        """Return whether the remote device is connected."""
        return bool(self._get("Connected", False))

    @property
    def services_resolved(self) -> bool:
        """Return whether BlueZ has resolved the remote GATT services."""
        return bool(self._get("ServicesResolved", False))

    @property
    def rssi(self) -> int | None:
        """Return the last received signal strength in dBm."""
        value = self._get("RSSI")
        return int(value) if isinstance(value, int) else None

    @property
    def tx_power(self) -> int | None:
        """Return the advertised transmit power in dBm."""
        value = self._get("TxPower")
        return int(value) if isinstance(value, int) else None

    @property
    def modalias(self) -> str | None:
        """Return remote Device ID information in modalias form."""
        value = self._get("Modalias")
        return value if isinstance(value, str) else None

    @property
    def legacy_pairing(self) -> bool:
        """Return whether the device uses a pre-2.1 pairing mechanism."""
        return bool(self._get("LegacyPairing", False))

    @property
    def trusted(self) -> bool:
        """Return whether the remote device is trusted."""
        return bool(self._get("Trusted", False))

    @trusted.setter
    def trusted(self, value: bool) -> None:
        """Set whether the remote device is trusted."""
        self._set_bool("Trusted", value)

    @property
    def blocked(self) -> bool:
        """Return whether the remote device is blocked."""
        return bool(self._get("Blocked", False))

    @blocked.setter
    def blocked(self, value: bool) -> None:
        """Set whether the remote device is blocked."""
        self._set_bool("Blocked", value)

    @property
    def alias(self) -> str:
        """Return the user-facing remote-device alias."""
        return str(self._get("Alias", ""))

    @alias.setter
    def alias(self, value: str) -> None:
        """Set the user-facing remote-device alias."""
        self._set("Alias", value, "s")

    def _set_bool(self, name: str, value: bool) -> None:
        """Set one boolean remote-device property."""
        self._set(name, value, "b")

    @property
    def manufacturer_data(self) -> dict[int, bytes]:
        """Return manufacturer identifiers mapped to advertisement payloads."""
        value = self._get("ManufacturerData", {})
        if not isinstance(value, Mapping):
            return {}
        result: dict[int, bytes] = {}
        for key, payload in value.items():
            try:
                result[int(key)] = bytes(cast(Any, payload))
            except (TypeError, ValueError):
                continue
        return result

    @property
    def service_data(self) -> dict[str, bytes]:
        """Return service UUIDs mapped to advertisement payloads."""
        value = self._get("ServiceData", {})
        if not isinstance(value, Mapping):
            return {}
        result: dict[str, bytes] = {}
        for key, payload in value.items():
            try:
                result[str(key)] = bytes(cast(Any, payload))
            except (TypeError, ValueError):
                continue
        return result

    @property
    def adapter(self) -> str:
        """Return the address of the local adapter owning this device."""
        return self._adapter_address

    @property
    def services_available(self) -> list[str]:
        """Return GATT service UUIDs currently exposed for this device."""
        return _runtime.run(self._services_available_async())

    async def _services_available_async(self) -> list[str]:
        """Collect service UUIDs below this device on the runtime loop."""
        managed = await _dbus.get_managed_objects(self._object_manager)
        services: list[str] = []
        for path, interfaces in managed.items():
            properties = interfaces.get(GATT_SERVICE_IFACE)
            if properties is None:
                continue
            parent = properties.get("Device")
            if parent != self._path and not (
                parent is None and path.startswith(f"{self._path}/")
            ):
                continue
            uuid = properties.get("UUID")
            if isinstance(uuid, str):
                services.append(uuid)
        return services

    def pair(self) -> None:
        """Pair with the remote device."""
        _runtime.run(self._pair_async())

    def cancel_pairing(self) -> None:
        """Cancel an in-progress pairing request."""
        _runtime.run(self._cancel_pairing_async())

    async def _pair_async(self) -> None:
        """Pair with the remote device on the runtime loop."""
        await cast(Any, self._manager).call_pair()

    async def _cancel_pairing_async(self) -> None:
        """Cancel pairing on the runtime loop."""
        await cast(Any, self._manager).call_cancel_pairing()

    def connect(self, profile: str | None = None, timeout: float = 35.0) -> None:
        """Connect to the device or an optional profile within ``timeout`` seconds."""
        _runtime.run(
            self._connect_async(profile, float(timeout)),
            timeout=float(timeout) + _METHOD_TIMEOUT_MARGIN,
        )

    async def _connect_async(self, profile: str | None, timeout: float) -> None:
        """Connect on the runtime loop and recover the BlueZ connecting state."""
        try:
            if profile is None:
                await asyncio.wait_for(self._connect_device_async(), timeout)
            else:
                await asyncio.wait_for(
                    self._connect_profile_async(profile), timeout
                )
        except (asyncio.TimeoutError, DBusError) as error:
            timed_out = isinstance(error, asyncio.TimeoutError) or (
                isinstance(error, DBusError) and "NoReply" in str(error.type)
            )
            if not timed_out:
                raise
            try:
                await self._disconnect_async()
            except (DBusError, OSError):
                pass
            raise BluetoothTimeoutError(
                f"Bluetooth connection timed out after {timeout:g} seconds"
            ) from error

    def disconnect(self) -> None:
        """Disconnect from the remote device."""
        _runtime.run(self._disconnect_async())

    async def _connect_device_async(self) -> None:
        """Connect to the remote device on the runtime loop."""
        await cast(Any, self._manager).call_connect()

    async def _connect_profile_async(self, profile: str) -> None:
        """Connect to one remote-device profile on the runtime loop."""
        await cast(Any, self._manager).call_connect_profile(profile)

    async def _disconnect_async(self) -> None:
        """Disconnect from the remote device on the runtime loop."""
        await cast(Any, self._manager).call_disconnect()


__all__ = ["Device"]
