"""Dara-owned wrappers for exporting a local Bluetooth GATT server."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from typing import Annotated, Any, cast

from dbus_fast.annotations import (
    DBusBool,
    DBusBytes,
    DBusDict,
    DBusObjectPath,
    DBusSignature,
    DBusStr,
)
from dbus_fast.service import ServiceInterface, dbus_property, method
from dbus_fast.constants import PropertyAccess

from . import _dbus, _runtime
from ._constants import (
    DARA_DBUS_NAME,
    DARA_DBUS_OBJECT,
    GATT_CHRC_IFACE,
    GATT_DESC_IFACE,
    GATT_SERVICE_IFACE,
)
from .errors import GattError
from .enums import CharacteristicFlag, DescriptorFlag


_ByteData = bytes | bytearray | memoryview | Sequence[int]
_StringArray = Annotated[list[str], DBusSignature("as")]
_DBusNone = Annotated[None, DBusSignature("")]


def _native_options(options: Mapping[str, object] | None) -> dict[str, object]:
    """Copy options while ensuring nested variants stay behind the boundary."""
    if options is None:
        return {}
    return cast(dict[str, object], _dbus.unwrap(dict(options)))


class _ApplicationRootInterface(ServiceInterface):
    """Private marker causing dbus-fast to expose ObjectManager at the root."""

    def __init__(self) -> None:
        """Create the private application-root interface."""
        super().__init__(f"{DARA_DBUS_NAME}.Application1")


class _LocalServiceInterface(ServiceInterface):
    """Private D-Bus interface backed by a Dara local-service wrapper."""

    def __init__(self, owner: LocalService) -> None:
        """Bind the interface to its Dara owner."""
        super().__init__(GATT_SERVICE_IFACE)
        self._owner = owner

    @dbus_property(access=PropertyAccess.READ, name="UUID")
    def uuid(self) -> DBusStr:
        """Return the service UUID for D-Bus clients."""
        return self._owner.uuid

    @dbus_property(access=PropertyAccess.READ, name="Primary")
    def primary(self) -> DBusBool:
        """Return the primary-service state for D-Bus clients."""
        return self._owner.primary


class _LocalCharacteristicInterface(ServiceInterface):
    """Private D-Bus interface backed by a Dara local characteristic."""

    def __init__(self, owner: LocalCharacteristic) -> None:
        """Bind the interface to its Dara owner."""
        super().__init__(GATT_CHRC_IFACE)
        self._owner = owner

    @dbus_property(access=PropertyAccess.READ, name="UUID")
    def uuid(self) -> DBusStr:
        """Return the characteristic UUID for D-Bus clients."""
        return self._owner.uuid

    @dbus_property(access=PropertyAccess.READ, name="Service")
    def service(self) -> DBusObjectPath:
        """Return the parent service path for D-Bus clients."""
        return self._owner._service_path

    @dbus_property(access=PropertyAccess.READ, name="Value")
    def value(self) -> DBusBytes:
        """Return the current characteristic value for D-Bus clients."""
        return self._owner.value

    @dbus_property(access=PropertyAccess.READ, name="Notifying")
    def notifying(self) -> DBusBool:
        """Return the notification state for D-Bus clients."""
        return self._owner.notifying

    @dbus_property(access=PropertyAccess.READ, name="Flags")
    def flags(self) -> _StringArray:
        """Return characteristic flag wire values for D-Bus clients."""
        return [flag.value for flag in self._owner.flags]

    @method(name="ReadValue")
    async def read_value(self, options: DBusDict) -> DBusBytes:
        """Handle a BlueZ characteristic read request."""
        return await self._owner._read_async(
            cast(dict[str, object], _dbus.unwrap(options))
        )

    @method(name="WriteValue")
    async def write_value(self, value: DBusBytes, options: DBusDict) -> _DBusNone:
        """Handle a BlueZ characteristic write request."""
        await self._owner._write_async(
            bytes(value),
            cast(dict[str, object], _dbus.unwrap(options)),
        )

    @method(name="StartNotify")
    async def start_notify(self) -> _DBusNone:
        """Handle a BlueZ request to start notifications."""
        await self._owner._set_notifying_async(True)

    @method(name="StopNotify")
    async def stop_notify(self) -> _DBusNone:
        """Handle a BlueZ request to stop notifications."""
        await self._owner._set_notifying_async(False)


class _LocalDescriptorInterface(ServiceInterface):
    """Private D-Bus interface backed by a Dara local descriptor."""

    def __init__(self, owner: LocalDescriptor) -> None:
        """Bind the interface to its Dara owner."""
        super().__init__(GATT_DESC_IFACE)
        self._owner = owner

    @dbus_property(access=PropertyAccess.READ, name="UUID")
    def uuid(self) -> DBusStr:
        """Return the descriptor UUID for D-Bus clients."""
        return self._owner.uuid

    @dbus_property(access=PropertyAccess.READ, name="Characteristic")
    def characteristic(self) -> DBusObjectPath:
        """Return the parent characteristic path for D-Bus clients."""
        return self._owner._characteristic_path

    @dbus_property(access=PropertyAccess.READ, name="Value")
    def value(self) -> DBusBytes:
        """Return the current descriptor value for D-Bus clients."""
        return self._owner.value

    @dbus_property(access=PropertyAccess.READ, name="Flags")
    def flags(self) -> _StringArray:
        """Return descriptor flag wire values for D-Bus clients."""
        return [flag.value for flag in self._owner.flags]

    @method(name="ReadValue")
    async def read_value(self, options: DBusDict) -> DBusBytes:
        """Handle a BlueZ descriptor read request."""
        return self._owner.read_value(cast(dict[str, object], _dbus.unwrap(options)))

    @method(name="WriteValue")
    async def write_value(self, value: DBusBytes, options: DBusDict) -> _DBusNone:
        """Handle a BlueZ descriptor write request."""
        self._owner.write_value(
            bytes(value),
            cast(dict[str, object], _dbus.unwrap(options)),
        )


class LocalService:
    """Dara-owned local GATT service definition."""

    def __init__(self, service_id: int, uuid: str, primary: bool = True) -> None:
        """Create a service with a stable numeric path component."""
        self._service_id = service_id
        self._uuid = uuid
        self._primary = primary
        self._path = f"{DARA_DBUS_OBJECT}/service{service_id:04d}"
        self._interface = _LocalServiceInterface(self)

    @property
    def uuid(self) -> str:
        """Return the Bluetooth UUID of this local service."""
        return self._uuid

    @property
    def primary(self) -> bool:
        """Return whether this is a primary GATT service."""
        return self._primary

    def get_path(self) -> str:
        """Return the service's D-Bus object path as a native string."""
        return self._path

    def _properties_native(self) -> dict[str, object]:
        """Return the native property registry used by the application."""
        return {"UUID": self._uuid, "Primary": self._primary}


class LocalCharacteristic:
    """Dara-owned local GATT characteristic definition."""

    read_callback: Callable[..., _ByteData] | None
    """Callback used to produce a value for remote read requests."""
    write_callback: Callable[..., Any] | None
    """Callback receiving native bytes and options for remote writes."""
    notify_callback: Callable[..., Any] | None
    """Callback receiving notification state and this characteristic."""

    def __init__(
        self,
        service_id: int,
        characteristic_id: int,
        uuid: str,
        value: _ByteData = b"",
        notifying: bool = False,
        flags: Sequence[CharacteristicFlag] = (),
        read_callback: Callable[..., _ByteData] | None = None,
        write_callback: Callable[..., Any] | None = None,
        notify_callback: Callable[..., Any] | None = None,
    ) -> None:
        """Create a characteristic below a numeric local service path."""
        self._service_id = service_id
        self._characteristic_id = characteristic_id
        self._uuid = uuid
        self._value = bytes(value)
        self._notifying = notifying
        self._flags = tuple(flags)
        self.read_callback = read_callback
        self.write_callback = write_callback
        self.notify_callback = notify_callback
        self._service_path = f"{DARA_DBUS_OBJECT}/service{service_id:04d}"
        self._path = f"{self._service_path}/char{characteristic_id:04d}"
        self._interface = _LocalCharacteristicInterface(self)

    @property
    def uuid(self) -> str:
        """Return the Bluetooth UUID of this local characteristic."""
        return self._uuid

    @property
    def value(self) -> bytes:
        """Return the current immutable characteristic value."""
        return self._value

    @property
    def notifying(self) -> bool:
        """Return whether a remote client has enabled notifications."""
        return self._notifying

    @property
    def is_notifying(self) -> bool:
        """Return whether a remote client has enabled notifications."""
        return self._notifying

    @property
    def flags(self) -> list[CharacteristicFlag]:
        """Return the characteristic's Dara-owned capability flags."""
        return list(self._flags)

    def get_path(self) -> str:
        """Return the characteristic's D-Bus object path as a native string."""
        return self._path

    def set_value(self, value: _ByteData) -> None:
        """Set the value and notify subscribed remote clients."""
        self._value = bytes(value)
        if self._notifying:
            self._interface.emit_properties_changed({"Value": self._value})

    def read_value(self, options: Mapping[str, object] | None = None) -> bytes:
        """Process a local read request using only native option values."""
        native_options = _native_options(options)
        if self.read_callback is not None:
            self.set_value(
                bytes(self.read_callback(native_options))
            )
        return self._value

    async def _read_async(self, options: dict[str, object]) -> bytes:
        """Process a D-Bus read."""
        if self.read_callback is not None:
            self.set_value(bytes(cast(Any, self.read_callback(options))))
        return self._value

    def write_value(
        self,
        value: _ByteData,
        options: Mapping[str, object] | None = None,
    ) -> None:
        """Process a local write request using bytes and native options."""
        native_value = bytes(value)
        native_options = _native_options(options)
        if self.write_callback is not None:
            self.write_callback(native_value, native_options)
        self.set_value(native_value)

    async def _write_async(
        self,
        value: bytes,
        options: dict[str, object],
    ) -> None:
        """Process a D-Bus write."""
        if self.write_callback is not None:
            self.write_callback(value, options)
        self.set_value(value)

    def start_notify(self) -> None:
        """Enable notifications and invoke the configured state callback."""
        self._set_notifying(True)

    def stop_notify(self) -> None:
        """Disable notifications and invoke the configured state callback."""
        self._set_notifying(False)

    def _set_notifying(self, state: bool) -> None:
        """Update notification state and synchronously invoke user code."""
        if self._notifying == state:
            return
        self._notifying = state
        self._interface.emit_properties_changed({"Notifying": state})
        if self.notify_callback is not None:
            self.notify_callback(state, self)

    async def _set_notifying_async(self, state: bool) -> None:
        """Update notification state and dispatch D-Bus-triggered user code."""
        if self._notifying == state:
            return
        self._notifying = state
        self._interface.emit_properties_changed({"Notifying": state})
        if self.notify_callback is not None:
            self.notify_callback(state, self)

    def _properties_native(self) -> dict[str, object]:
        """Return the native property registry used by the application."""
        return {
            "UUID": self._uuid,
            "Service": self._service_path,
            "Value": self._value,
            "Notifying": self._notifying,
            "Flags": [flag.value for flag in self._flags],
        }


class LocalDescriptor:
    """Dara-owned local GATT descriptor definition."""

    def __init__(
        self,
        service_id: int,
        characteristic_id: int,
        descriptor_id: int,
        uuid: str,
        value: _ByteData = b"",
        flags: Sequence[DescriptorFlag] = (),
    ) -> None:
        """Create a descriptor below a numeric local characteristic path."""
        self._service_id = service_id
        self._characteristic_id = characteristic_id
        self._descriptor_id = descriptor_id
        self._uuid = uuid
        self._value = bytes(value)
        self._flags = tuple(flags)
        service_path = f"{DARA_DBUS_OBJECT}/service{service_id:04d}"
        self._characteristic_path = f"{service_path}/char{characteristic_id:04d}"
        self._path = f"{self._characteristic_path}/desc{descriptor_id:04d}"
        self._interface = _LocalDescriptorInterface(self)

    @property
    def uuid(self) -> str:
        """Return the Bluetooth UUID of this local descriptor."""
        return self._uuid

    @property
    def value(self) -> bytes:
        """Return the current immutable descriptor value."""
        return self._value

    @property
    def flags(self) -> list[DescriptorFlag]:
        """Return the descriptor's Dara-owned capability flags."""
        return list(self._flags)

    def get_path(self) -> str:
        """Return the descriptor's D-Bus object path as a native string."""
        return self._path

    def read_value(self, options: Mapping[str, object] | None = None) -> bytes:
        """Return descriptor bytes for a read request with native options."""
        _native_options(options)
        return self._value

    def write_value(
        self,
        value: _ByteData,
        options: Mapping[str, object] | None = None,
    ) -> None:
        """Store descriptor bytes from a write request with native options."""
        _native_options(options)
        self._value = bytes(value)
        self._interface.emit_properties_changed({"Value": self._value})

    def _properties_native(self) -> dict[str, object]:
        """Return the native property registry used by the application."""
        return {
            "UUID": self._uuid,
            "Characteristic": self._characteristic_path,
            "Value": self._value,
            "Flags": [flag.value for flag in self._flags],
        }


_ManagedObject = LocalService | LocalCharacteristic | LocalDescriptor


class Application:
    """Own and export a complete local GATT application object tree."""

    def __init__(self) -> None:
        """Create an empty application at Dara's stable Bluetooth root path."""
        self._path = DARA_DBUS_OBJECT
        self._objects: list[_ManagedObject] = []
        self._root_interface = _ApplicationRootInterface()
        self._exported = False
        self._bus: Any = None

    def add_managed_object(self, service_object: _ManagedObject) -> None:
        """Add a Dara local service, characteristic, or descriptor."""
        self._objects.append(service_object)
        if self._exported:
            _runtime.run(self._export_object_async(service_object))

    def get_path(self) -> str:
        """Return the local application's D-Bus root path as a native string."""
        return self._path

    def get_managed_objects(self) -> dict[str, dict[str, dict[str, object]]]:
        """Return a native-value snapshot of every managed GATT object."""
        return {
            item.get_path(): {item._interface.name: item._properties_native()}
            for item in self._objects
        }

    def export(self) -> None:
        """Export the application and its managed objects on the shared bus."""
        _runtime.run(self._export_async())

    async def _export_async(self) -> None:
        """Export the complete application tree on the runtime loop."""
        if self._exported:
            return
        bus = await _runtime.get_bus()
        bus.export(self._path, self._root_interface)
        self._bus = bus
        for item in self._objects:
            bus.export(item.get_path(), item._interface)
        self._exported = True

    async def _export_object_async(self, item: _ManagedObject) -> None:
        """Export one object added after the application was published."""
        if self._bus is None:
            raise GattError("local GATT application is not exported")
        self._bus.export(item.get_path(), item._interface)

    def unexport(self) -> None:
        """Remove every local GATT object from the shared bus."""
        _runtime.run(self._unexport_async())

    async def _unexport_async(self) -> None:
        """Unexport the complete application tree on the runtime loop."""
        if not self._exported or self._bus is None:
            return
        for item in reversed(self._objects):
            self._bus.unexport(item.get_path(), item._interface)
        self._bus.unexport(self._path, self._root_interface)
        self._bus = None
        self._exported = False


__all__ = [
    "Application",
    "LocalCharacteristic",
    "LocalDescriptor",
    "LocalService",
]
