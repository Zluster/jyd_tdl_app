"""Local Bluetooth adapter management through the BlueZ D-Bus API."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Sequence
from contextlib import contextmanager
from typing import TYPE_CHECKING, Any, Iterator, cast

from dbus_fast.errors import DBusError

from . import _dbus, _runtime
from ._constants import (
    ADAPTER_INTERFACE,
    DBUS_OM_IFACE,
    DBUS_PROP_IFACE,
    DEVICE_INTERFACE,
)
from .errors import AdapterNotFoundError, BluetoothError, DeviceNotFoundError


_Callback = Callable[..., Any]
_METHOD_TIMEOUT_MARGIN = 5.0

if TYPE_CHECKING:
    from .device import Device


def list_adapters() -> list[str]:
    """Return the addresses of all available local Bluetooth adapters."""
    return [adapter.address for adapter in Adapter.available()]


class Adapter:
    """Manage one local Bluetooth adapter exposed by BlueZ."""

    on_device_found: _Callback | None
    """Callback invoked when discovery reports a device.

    The callback receives the discovered :class:`Device`.
    """

    on_connect: _Callback | None
    """Callback invoked when a device connects to this adapter."""

    on_disconnect: _Callback | None
    """Callback invoked when a device disconnects from this adapter."""

    @classmethod
    def available(cls) -> list[Adapter]:
        """Return wrappers for all local Bluetooth adapters."""
        bus = _runtime.run(_runtime.get_bus())
        proxy = _runtime.run(_dbus.get_proxy(bus, "/"))
        manager = proxy.get_interface(DBUS_OM_IFACE)
        managed = _runtime.run(_dbus.get_managed_objects(manager))
        addresses = [
            str(properties.get("Address", ""))
            for interfaces in managed.values()
            if (properties := interfaces.get(ADAPTER_INTERFACE)) is not None
            and properties.get("Address")
        ]
        return [cls(address) for address in addresses]

    def __init__(self, address: str | None = None) -> None:
        """Resolve ``address``, or the first available adapter when omitted."""
        self.on_device_found = None
        self.on_connect = None
        self.on_disconnect = None
        self._device_signals: dict[str, tuple[Any, _Callback, Device]] = {}
        self._scan_devices: dict[str, Device] | None = None
        self._path, self._address = _runtime.run(self._initialize(address))

    async def _initialize(self, address: str | None) -> tuple[str, str]:
        """Resolve this adapter and register discovery signal handlers."""
        bus = await _runtime.get_bus()
        self._bus = bus
        root = await _dbus.get_proxy(bus, "/")
        manager = root.get_interface(DBUS_OM_IFACE)
        managed = await _dbus.get_managed_objects(manager)
        path: str | None
        if address is None:
            path = next(
                (
                    object_path
                    for object_path, interfaces in managed.items()
                    if ADAPTER_INTERFACE in interfaces
                ),
                None,
            )
        else:
            path = _dbus.find_object_path(managed, adapter=address)
        if path is None:
            requested = f" {address}" if address is not None else ""
            raise AdapterNotFoundError(f"Bluetooth adapter{requested} was not found")

        properties = managed[path][ADAPTER_INTERFACE]
        resolved_address = properties.get("Address")
        if not isinstance(resolved_address, str) or not resolved_address:
            raise AdapterNotFoundError("Bluetooth adapter has no usable address")

        cast(Any, manager).on_interfaces_added(self._interfaces_added)
        cast(Any, manager).on_interfaces_removed(self._interfaces_removed)
        self._object_manager = manager

        self._proxy = await _dbus.get_proxy(bus, path)
        self._manager = self._proxy.get_interface(ADAPTER_INTERFACE)
        self._properties = self._proxy.get_interface(DBUS_PROP_IFACE)
        self._path = path
        self._address = resolved_address

        for object_path, interfaces in managed.items():
            device = interfaces.get(DEVICE_INTERFACE)
            if device is not None and self._owns_device(object_path, device, path):
                await self._subscribe_device(
                    object_path,
                    str(device.get("Address") or _dbus.address_from_path(object_path)),
                )
        return path, resolved_address

    @staticmethod
    def _owns_device(
        path: str,
        properties: dict[str, Any],
        adapter_path: str,
    ) -> bool:
        """Return whether device properties identify the expected adapter."""
        parent = properties.get("Adapter")
        return parent == adapter_path or (
            parent is None and path.startswith(f"{adapter_path}/")
        )

    async def _subscribe_device(self, path: str, address: str) -> Device:
        """Listen for connection changes on one device object."""
        if path in self._device_signals:
            return self._device_signals[path][2]
        proxy = await _dbus.get_proxy(self._bus, path)
        properties = proxy.get_interface(DBUS_PROP_IFACE)
        manager = proxy.get_interface(DEVICE_INTERFACE)

        from .device import Device

        device = Device._resolved(
            self._address,
            address,
            path,
            self._bus,
            self._object_manager,
            manager,
            properties,
        )

        def changed(
            interface: str,
            values: dict[str, Any],
            invalidated: list[str],
        ) -> None:
            """Attach the object path to a device property signal."""
            self._properties_changed(path, interface, values, invalidated)

        cast(Any, properties).on_properties_changed(changed)
        self._device_signals[path] = (properties, changed, device)
        return device

    def _dispatch(
        self, callback: _Callback | None, device: Device
    ) -> None:
        """Invoke a device callback when configured."""
        if callback is not None:
            callback(device)

    def _interfaces_added(
        self,
        path: str,
        interfaces: dict[str, dict[str, Any]],
    ) -> None:
        """Handle a BlueZ object appearing during discovery."""
        native = cast(dict[str, dict[str, Any]], _dbus.unwrap(interfaces))
        device = native.get(DEVICE_INTERFACE)
        if device is None or not self._owns_device(path, device, self._path):
            return

        address = device.get("Address") or _dbus.address_from_path(path)
        if not isinstance(address, str) or not address:
            return
        asyncio.create_task(self._add_device(path, address, bool(device.get("Connected"))))

    async def _add_device(self, path: str, address: str, connected: bool) -> None:
        """Initialize a newly announced device before dispatching callbacks."""
        device = await self._subscribe_device(path, address)
        if self._scan_devices is not None:
            self._scan_devices[address] = device
        self._dispatch(self.on_device_found, device)
        if connected:
            self._dispatch(self.on_connect, device)

    def _interfaces_removed(self, path: str, interfaces: Sequence[str]) -> None:
        """Forget signal handlers for a BlueZ device object that disappeared."""
        if DEVICE_INTERFACE not in interfaces:
            return
        subscription = self._device_signals.pop(path, None)
        if subscription is not None:
            properties, handler, _device = subscription
            cast(Any, properties).off_properties_changed(handler)

    def _properties_changed(
        self,
        path: str,
        interface: str,
        changed: dict[str, Any],
        _invalidated: Sequence[str],
    ) -> None:
        """Dispatch device connection-state changes to user callbacks."""
        if interface != DEVICE_INTERFACE:
            return
        native = cast(dict[str, Any], _dbus.unwrap(changed))
        connected = native.get("Connected")
        if not isinstance(connected, bool):
            return
        device = self._device_signals[path][2]
        self._dispatch(
            self.on_connect if connected else self.on_disconnect, device
        )

    def _get(self, name: str, default: Any = None) -> Any:
        """Read one adapter property synchronously."""
        return _runtime.run(self._get_async(name, default))

    async def _get_async(self, name: str, default: Any) -> Any:
        """Read one adapter property on the runtime loop."""
        return await _dbus.get_prop(self._properties, ADAPTER_INTERFACE, name, default)

    def _set(self, name: str, value: object, signature: str) -> None:
        """Set one adapter property synchronously."""
        _runtime.run(self._set_async(name, value, signature))

    async def _set_async(self, name: str, value: object, signature: str) -> None:
        """Set one adapter property on the runtime loop."""
        await _dbus.set_prop(
            self._properties, ADAPTER_INTERFACE, name, value, signature
        )

    @property
    def address(self) -> str:
        """Return the adapter MAC address."""
        return self._address

    @property
    def name(self) -> str:
        """Return the system name of the adapter."""
        return str(self._get("Name", ""))

    @property
    def alias(self) -> str:
        """Return the user-facing adapter alias."""
        return str(self._get("Alias", ""))

    @alias.setter
    def alias(self, value: str) -> None:
        """Set the user-facing adapter alias."""
        self._set("Alias", value, "s")

    @property
    def powered(self) -> bool:
        """Return whether the adapter is powered."""
        return bool(self._get("Powered", False))

    @powered.setter
    def powered(self, value: bool) -> None:
        """Set whether the adapter is powered."""
        self._set_bool("Powered", value)

    @property
    def pairable(self) -> bool:
        """Return whether the adapter accepts pairing requests."""
        return bool(self._get("Pairable", False))

    @pairable.setter
    def pairable(self, value: bool) -> None:
        """Set whether the adapter accepts pairing requests."""
        self._set_bool("Pairable", value)

    @property
    def pairable_timeout(self) -> int:
        """Return the pairable timeout in seconds, where zero means unlimited."""
        return int(self._get("PairableTimeout", 0))

    @pairable_timeout.setter
    def pairable_timeout(self, value: int) -> None:
        """Set the pairable timeout in seconds, where zero means unlimited."""
        self._set_uint32("PairableTimeout", value)

    @property
    def discoverable(self) -> bool:
        """Return whether the adapter is discoverable."""
        return bool(self._get("Discoverable", False))

    @discoverable.setter
    def discoverable(self, value: bool) -> None:
        """Set whether the adapter is discoverable."""
        self._set_bool("Discoverable", value)

    @property
    def discoverable_timeout(self) -> int:
        """Return the discoverable timeout in seconds, where zero is unlimited."""
        return int(self._get("DiscoverableTimeout", 0))

    @discoverable_timeout.setter
    def discoverable_timeout(self, value: int) -> None:
        """Set the discoverable timeout in seconds, where zero is unlimited."""
        self._set_uint32("DiscoverableTimeout", value)

    @property
    def discovering(self) -> bool:
        """Return whether device discovery is active."""
        return bool(self._get("Discovering", False))

    @property
    def uuids(self) -> list[str]:
        """Return service UUIDs provided by the local adapter."""
        return [str(value) for value in self._get("UUIDs", [])]

    @property
    def bt_class(self) -> int:
        """Return the Bluetooth class of the adapter."""
        return int(self._get("Class", 0))

    def _set_bool(self, name: str, value: bool) -> None:
        """Set one boolean adapter property."""
        self._set(name, value, "b")

    def _set_uint32(self, name: str, value: int) -> None:
        """Set one unsigned 32-bit adapter property."""
        self._set(name, value, "u")

    def get_all(self) -> dict[str, Any]:
        """Return all adapter properties as native Python values."""
        return _runtime.run(self._get_all_async())

    async def _get_all_async(self) -> dict[str, Any]:
        """Read every adapter property on the runtime loop."""
        return await _dbus.get_all_props(self._properties, ADAPTER_INTERFACE)

    def set_discovery_filter(
        self,
        *,
        uuids: Sequence[str] | None = None,
        rssi: int | None = None,
        pathloss: int | None = None,
        transport: str | None = None,
        duplicate_data: bool | None = None,
        discoverable: bool | None = None,
        pattern: str | None = None,
    ) -> None:
        """Set BlueZ discovery filters; omitted values leave no filter entry."""
        filters: dict[str, object] = {}
        if uuids is not None:
            filters["UUIDs"] = list(uuids)
        if rssi is not None:
            filters["RSSI"] = rssi
        if pathloss is not None:
            filters["Pathloss"] = pathloss
        if transport is not None:
            filters["Transport"] = transport
        for key, value in (
            ("DuplicateData", duplicate_data),
            ("Discoverable", discoverable),
        ):
            if value is not None:
                filters[key] = value
        if pattern is not None:
            filters["Pattern"] = pattern
        _runtime.run(self._set_discovery_filter_async(filters))

    async def _set_discovery_filter_async(self, filters: dict[str, object]) -> None:
        """Pack and send one discovery filter on the runtime loop."""
        await cast(Any, self._manager).call_set_discovery_filter(
            _dbus.pack_options(filters)
        )

    def show_duplicates(self) -> None:
        """Report duplicate advertisements during device discovery."""
        self.set_discovery_filter(duplicate_data=True)

    def hide_duplicates(self) -> None:
        """Suppress duplicate advertisements during device discovery."""
        self.set_discovery_filter(duplicate_data=False)

    def start_discovery(self) -> None:
        """Start asynchronous discovery of nearby Bluetooth devices."""
        _runtime.run(self._start_discovery_async())

    def stop_discovery(self) -> None:
        """Stop Bluetooth device discovery."""
        _runtime.run(self._stop_discovery_async())

    async def _start_discovery_async(self) -> None:
        """Start discovery on the runtime loop."""
        await cast(Any, self._manager).call_start_discovery()

    async def _stop_discovery_async(self) -> None:
        """Stop discovery on the runtime loop."""
        await cast(Any, self._manager).call_stop_discovery()

    def nearby_discovery(self, timeout: float = 10.0) -> list[Device]:
        """Scan for ``timeout`` seconds and return newly reported devices."""
        return _runtime.run(
            self._nearby_discovery_async(float(timeout)),
            timeout=float(timeout) + _METHOD_TIMEOUT_MARGIN,
        )

    async def _nearby_discovery_async(self, timeout: float) -> list[Device]:
        """Collect device addresses while discovery runs on the event loop."""
        self._scan_devices = {}
        await self._start_discovery_async()
        try:
            await asyncio.sleep(timeout)
        finally:
            try:
                await self._stop_discovery_async()
            finally:
                devices = list(self._scan_devices.values())
                self._scan_devices = None
        return devices
    
    @contextmanager
    def nearby_discovery_scoped(self) -> Iterator[None]:
        _runtime.run(self._start_discovery_async())
        try:
            yield
        finally:
            _runtime.run(self._stop_discovery_async())

    def remove_device(self, path: str) -> None:
        """Remove the remote device at a BlueZ object path from this adapter."""
        try:
            _runtime.run(self._remove_device_async(path))
        except BluetoothError as error:
            cause = error.__cause__
            if isinstance(cause, DBusError) and "DoesNotExist" in str(cause.type):
                raise DeviceNotFoundError("Bluetooth device was not found") from error
            raise

    async def _remove_device_async(self, path: str) -> None:
        """Remove a remote device on the runtime loop."""
        await cast(Any, self._manager).call_remove_device(path)


__all__ = ["Adapter", "list_adapters"]
