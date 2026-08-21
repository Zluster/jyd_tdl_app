"""Remote Bluetooth GATT service, characteristic, and descriptor wrappers."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import Any, cast

from . import _dbus, _runtime
from ._constants import (
    DBUS_PROP_IFACE,
    GATT_CHRC_IFACE,
    GATT_DESC_IFACE,
    GATT_SERVICE_IFACE,
)
from .errors import GattError
from .device import Device
from .enums import CharacteristicFlag, DescriptorFlag


_ByteData = bytes | bytearray | memoryview | Sequence[int]


class _RemoteGattObject:
    """Shared lazy path resolution for remote GATT objects."""

    def __init__(
        self,
        adapter_addr: str,
        device_addr: str,
        service_uuid: str,
        *,
        characteristic_uuid: str | None = None,
        descriptor_uuid: str | None = None,
        interface: str,
    ) -> None:
        """Store selectors and resolve immediately when services are ready."""
        self._adapter_address = adapter_addr
        self._device_address = device_addr
        self._service_uuid = service_uuid
        self._characteristic_uuid = characteristic_uuid
        self._descriptor_uuid = descriptor_uuid
        self._interface = interface
        self._device = Device(adapter_addr, device_addr)
        self._path: str | None = None
        if self._device.services_resolved:
            self.resolve_gatt()

    def resolve_gatt(self) -> bool:
        """Resolve the GATT object after BlueZ has discovered its services."""
        if not self._device.services_resolved:
            raise GattError(
                f"GATT services are not resolved for {self._device_address}"
            )
        self._path, self._manager, self._properties = _runtime.run(
            self._initialize_dbus()
        )
        return True

    async def _initialize_dbus(self) -> tuple[str, Any, Any]:
        """Resolve this object's path and initialize its D-Bus interfaces."""
        managed = await _dbus.get_managed_objects(self._device._object_manager)
        path = _dbus.find_object_path(
            managed,
            adapter=self._adapter_address,
            device=self._device_address,
            service=self._service_uuid,
            characteristic=self._characteristic_uuid,
            descriptor=self._descriptor_uuid,
        )
        if path is None:
            description = (
                self._descriptor_uuid or self._characteristic_uuid or self._service_uuid
            )
            raise GattError(f"remote GATT object {description} was not found")
        proxy = await _dbus.get_proxy(self._device._bus, path)
        return (
            path,
            proxy.get_interface(self._interface),
            proxy.get_interface(DBUS_PROP_IFACE),
        )

    def _require_path(self) -> str:
        """Return the resolved path or resolve it on demand."""
        if self._path is None:
            self.resolve_gatt()
        assert self._path is not None
        return self._path

    def _get(self, name: str, default: Any = None) -> Any:
        """Read one GATT property synchronously."""
        self._require_path()
        return _runtime.run(self._get_async(name, default))

    async def _get_async(self, name: str, default: Any) -> Any:
        """Read one GATT property on the runtime loop."""
        return await _dbus.get_prop(self._properties, self._interface, name, default)

    async def _read_value_async(self, options: dict[str, Any]) -> Any:
        """Read a GATT value on the runtime loop."""
        return await cast(Any, self._manager).call_read_value(options)

    async def _write_value_async(
        self,
        value: bytes,
        options: dict[str, Any],
    ) -> None:
        """Write a GATT value on the runtime loop."""
        await cast(Any, self._manager).call_write_value(value, options)


class RemoteService(_RemoteGattObject):
    """Represent a GATT service belonging to a remote Bluetooth device."""

    def __init__(
        self,
        adapter_addr: str,
        device_addr: str,
        service_uuid: str,
    ) -> None:
        """Identify a remote service by adapter, device, and service UUID."""
        super().__init__(
            adapter_addr,
            device_addr,
            service_uuid,
            interface=GATT_SERVICE_IFACE,
        )

    @property
    def uuid(self) -> str:
        """Return the service UUID reported by BlueZ."""
        return str(self._get("UUID", self._service_uuid))

    @property
    def device(self) -> Device:
        """Return the Dara remote-device wrapper owning this service."""
        return self._device

    @property
    def primary(self) -> bool:
        """Return whether this is a primary GATT service."""
        return bool(self._get("Primary", False))


class RemoteCharacteristic(_RemoteGattObject):
    """Read, write, and subscribe to a remote GATT characteristic."""

    on_value_changed: Callable[[bytes], Any] | None
    """Callback receiving notification values."""

    def __init__(
        self,
        adapter_addr: str,
        device_addr: str,
        service_uuid: str,
        characteristic_uuid: str,
    ) -> None:
        """Identify a characteristic by its parent service and UUID."""
        self.on_value_changed = None
        self._value = b""
        self._notify_handler: Callable[..., None] | None = None
        super().__init__(
            adapter_addr,
            device_addr,
            service_uuid,
            characteristic_uuid=characteristic_uuid,
            interface=GATT_CHRC_IFACE,
        )

    @property
    def uuid(self) -> str:
        """Return the characteristic UUID reported by BlueZ."""
        return str(self._get("UUID", self._characteristic_uuid or ""))

    @property
    def value(self) -> bytes:
        """Return the value cached by the latest read or notification."""
        return self._value

    @property
    def notifying(self) -> bool:
        """Return whether notifications are enabled for this characteristic."""
        return bool(self._get("Notifying", False))

    @property
    def flags(self) -> list[CharacteristicFlag]:
        """Return characteristic capabilities as Dara-owned flags."""
        values = self._get("Flags", [])
        if not isinstance(values, list):
            return []
        flags: list[CharacteristicFlag] = []
        for value in values:
            try:
                flags.append(CharacteristicFlag(str(value)))
            except ValueError:
                continue
        return flags

    def read_value(self, offset: int = 0) -> bytes:
        """Read bytes from the characteristic starting at ``offset``."""
        self._require_path()
        value = _runtime.run(
            self._read_value_async(
                _dbus.pack_options({"offset": offset}),
            ),
        )
        try:
            self._value = bytes(cast(Any, _dbus.unwrap(value)))
        except (TypeError, ValueError) as error:
            raise GattError("BlueZ returned an invalid characteristic value") from error
        return self._value

    def write_value(
        self,
        data: bytes | bytearray | memoryview | Sequence[int],
        offset: int = 0,
        response: bool = True,
    ) -> None:
        """Write bytes using a request or write-without-response command."""
        value = bytes(data)
        self._require_path()
        _runtime.run(
            self._write_value_async(
                value,
                _dbus.pack_options(
                    {
                        "offset": offset,
                        "type": "request" if response else "command",
                    }
                ),
            ),
        )
        self._value = value

    def add_callback(self, callback: Callable[[bytes], Any] | None) -> None:
        """Set or clear the callback invoked for characteristic notifications."""
        self.on_value_changed = callback

    def start_notify(self) -> None:
        """Subscribe to characteristic value notifications."""
        self._require_path()
        _runtime.run(self._start_notify_async())

    async def _start_notify_async(self) -> None:
        """Register the value handler and start notifications on the loop."""
        if self._notify_handler is None:
            def changed(
                interface: str,
                values: dict[str, Any],
                invalidated: list[str],
            ) -> None:
                """Forward a PropertiesChanged notification to the wrapper."""
                self._properties_changed(interface, values, invalidated)

            cast(Any, self._properties).on_properties_changed(changed)
            self._notify_handler = changed
        try:
            await cast(Any, self._manager).call_start_notify()
        except Exception:
            self._remove_notify_handler()
            raise

    def _properties_changed(
        self,
        interface: str,
        changed: dict[str, Any],
        _invalidated: Sequence[str],
    ) -> None:
        """Update the cached value and dispatch a user notification callback."""
        if interface != GATT_CHRC_IFACE:
            return
        native = cast(dict[str, Any], _dbus.unwrap(changed))
        if "Value" not in native:
            return
        try:
            self._value = bytes(cast(Any, native["Value"]))
        except (TypeError, ValueError):
            return
        if self.on_value_changed is not None:
            self.on_value_changed(self._value)

    def stop_notify(self) -> None:
        """Stop characteristic value notifications and remove the signal handler."""
        self._require_path()
        try:
            _runtime.run(self._stop_notify_async())
        finally:
            self._remove_notify_handler()

    async def _stop_notify_async(self) -> None:
        """Stop characteristic notifications on the runtime loop."""
        await cast(Any, self._manager).call_stop_notify()

    def _remove_notify_handler(self) -> None:
        """Unregister the current PropertiesChanged handler when present."""
        if self._notify_handler is not None:
            cast(Any, self._properties).off_properties_changed(self._notify_handler)
        self._notify_handler = None


class RemoteDescriptor(_RemoteGattObject):
    """Read and write a descriptor on a remote GATT characteristic."""

    def __init__(
        self,
        adapter_addr: str,
        device_addr: str,
        service_uuid: str,
        characteristic_uuid: str,
        descriptor_uuid: str,
    ) -> None:
        """Identify a descriptor by its service, characteristic, and UUID."""
        self._value = b""
        super().__init__(
            adapter_addr,
            device_addr,
            service_uuid,
            characteristic_uuid=characteristic_uuid,
            descriptor_uuid=descriptor_uuid,
            interface=GATT_DESC_IFACE,
        )

    @property
    def uuid(self) -> str:
        """Return the descriptor UUID reported by BlueZ."""
        return str(self._get("UUID", self._descriptor_uuid or ""))

    @property
    def value(self) -> bytes:
        """Return the value cached by the latest descriptor read or write."""
        return self._value

    @property
    def flags(self) -> list[DescriptorFlag]:
        """Return descriptor capabilities as Dara-owned flags."""
        values = self._get("Flags", [])
        if not isinstance(values, list):
            return []
        flags: list[DescriptorFlag] = []
        for value in values:
            try:
                flags.append(DescriptorFlag(str(value)))
            except ValueError:
                continue
        return flags

    def read_value(self, offset: int = 0) -> bytes:
        """Read descriptor bytes starting at ``offset``."""
        self._require_path()
        value = _runtime.run(
            self._read_value_async(
                _dbus.pack_options({"offset": offset}),
            ),
        )
        try:
            self._value = bytes(cast(Any, _dbus.unwrap(value)))
        except (TypeError, ValueError) as error:
            raise GattError("BlueZ returned an invalid descriptor value") from error
        return self._value

    def write_value(
        self,
        data: bytes | bytearray | memoryview | Sequence[int],
        offset: int = 0,
    ) -> None:
        """Write descriptor bytes starting at ``offset``."""
        value = bytes(data)
        self._require_path()
        _runtime.run(
            self._write_value_async(
                value,
                _dbus.pack_options({"offset": offset}),
            ),
        )
        self._value = value


__all__ = ["RemoteCharacteristic", "RemoteDescriptor", "RemoteService"]
