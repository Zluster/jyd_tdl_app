"""Bluetooth LE advertisement data and registration wrappers."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from typing import Annotated, Any, cast

from dbus_fast import Variant
from dbus_fast.annotations import (
    DBusSignature,
    DBusStr,
    DBusUInt16,
)
from dbus_fast.constants import PropertyAccess
from dbus_fast.errors import DBusError
from dbus_fast.service import ServiceInterface, dbus_property, method

from . import _dbus, _runtime
from ._constants import (
    DARA_DBUS_OBJECT,
    LE_ADVERTISEMENT_IFACE,
    LE_ADVERTISING_MANAGER_IFACE,
)
from .adapter import Adapter
from .enums import AdvertisementType


_ByteData = bytes | bytearray | memoryview | Sequence[int]
_StringArray = Annotated[list[str], DBusSignature("as")]
_ManufacturerData = Annotated[dict[int, Variant], DBusSignature("a{qv}")]
_ServiceData = Annotated[dict[str, Variant], DBusSignature("a{sv}")]
_DBusNone = Annotated[None, DBusSignature("")]


class _AdvertisementInterface(ServiceInterface):
    """Private LEAdvertisement1 interface backed by a Dara wrapper."""

    def __init__(self, owner: Advertisement) -> None:
        """Bind the private service interface to its Dara owner."""
        super().__init__(LE_ADVERTISEMENT_IFACE)
        self._owner = owner

    @dbus_property(access=PropertyAccess.READ, name="Type")
    def advertisement_type(self) -> DBusStr:
        """Return the BlueZ advertisement type wire value."""
        return self._owner.advertisement_type.value

    @dbus_property(access=PropertyAccess.READ, name="ServiceUUIDs")
    def service_uuids(self) -> _StringArray:
        """Return advertised service UUIDs for D-Bus clients."""
        return self._owner.service_uuids

    @dbus_property(access=PropertyAccess.READ, name="ManufacturerData")
    def manufacturer_data(self) -> _ManufacturerData:
        """Pack manufacturer payloads into private D-Bus variants."""
        return {
            company_id: Variant("ay", data)
            for company_id, data in self._owner._manufacturer_data.items()
        }

    @dbus_property(access=PropertyAccess.READ, name="ServiceData")
    def service_data(self) -> _ServiceData:
        """Pack service payloads into private D-Bus variants."""
        return {
            uuid: Variant("ay", data)
            for uuid, data in self._owner.service_data.items()
        }

    @dbus_property(access=PropertyAccess.READ, name="Includes")
    def includes(self) -> _StringArray:
        """Return optional fields requested in the advertisement."""
        return self._owner.includes

    @dbus_property(access=PropertyAccess.READ, name="LocalName")
    def local_name(self) -> DBusStr:
        """Return the advertised local name or an empty value."""
        return self._owner.local_name or ""

    @dbus_property(access=PropertyAccess.READ, name="Appearance")
    def appearance(self) -> DBusUInt16:
        """Return the advertised appearance or zero when omitted."""
        return self._owner.appearance or 0

    @method(name="Release")
    async def release(self) -> _DBusNone:
        """Handle BlueZ releasing the advertisement."""
        await self._owner._release_async()


class Advertisement:
    """Dara-owned Bluetooth LE advertisement definition."""

    on_release: Callable[[], Any] | None
    """Callback invoked when BlueZ releases this advertisement."""

    def __init__(
        self,
        advert_id: int,
        advertisement_type: AdvertisementType,
    ) -> None:
        """Create a stable advertisement path for a Dara advertisement type."""
        self._advert_id = advert_id
        self._advertisement_type = advertisement_type
        self._service_uuids: list[str] = []
        self._manufacturer_data: dict[int, bytes] = {}
        self._service_data: dict[str, bytes] = {}
        self._includes: set[str] = set()
        self._local_name: str | None = None
        self._appearance: int | None = None
        self._released = False
        self._adapter: Adapter | None = None
        self._registered = False
        self.on_release = None
        self._path = f"{DARA_DBUS_OBJECT}/advertisement{advert_id:04d}"
        self._interface = _AdvertisementInterface(self)

    @property
    def advertisement_type(self) -> AdvertisementType:
        """Return whether this is a connectable or broadcast advertisement."""
        return self._advertisement_type

    @property
    def service_uuids(self) -> list[str]:
        """Return service UUIDs included in the advertisement."""
        return list(self._service_uuids)

    @service_uuids.setter
    def service_uuids(self, values: Sequence[str]) -> None:
        """Set service UUIDs included in the advertisement."""
        self._service_uuids = list(values)

    def manufacturer_data(self, company_id: int, data: _ByteData) -> None:
        """Add or replace manufacturer-specific advertisement data."""
        self._manufacturer_data[company_id] = bytes(data)

    @property
    def manufacturer_data_values(self) -> dict[int, bytes]:
        """Return a copy of manufacturer-specific advertisement data."""
        return dict(self._manufacturer_data)

    @property
    def service_data(self) -> dict[str, bytes]:
        """Return a copy of service-specific advertisement data."""
        return dict(self._service_data)

    @service_data.setter
    def service_data(self, values: Mapping[str, _ByteData]) -> None:
        """Set service-specific advertisement payloads."""
        self._service_data = {uuid: bytes(data) for uuid, data in values.items()}

    @property
    def includes(self) -> list[str]:
        """Return optional controller-generated advertisement fields."""
        return sorted(self._includes)

    @property
    def include_tx_power(self) -> bool:
        """Return whether the controller should include transmit power."""
        return "tx-power" in self._includes

    @include_tx_power.setter
    def include_tx_power(self, state: bool) -> None:
        """Set whether the controller should include transmit power."""
        if state:
            self._includes.add("tx-power")
        else:
            self._includes.discard("tx-power")

    @property
    def local_name(self) -> str | None:
        """Return the local name included in the advertisement."""
        return self._local_name

    @local_name.setter
    def local_name(self, name: str | None) -> None:
        """Set or omit the local name included in the advertisement."""
        self._local_name = name

    @property
    def appearance(self) -> int | None:
        """Return the Generic Access Profile appearance value when set."""
        return self._appearance

    @appearance.setter
    def appearance(self, value: int | None) -> None:
        """Set or omit the Generic Access Profile appearance value."""
        self._appearance = value

    @property
    def released(self) -> bool:
        """Return whether BlueZ has released this advertisement."""
        return self._released

    def get_path(self) -> str:
        """Return the advertisement's D-Bus object path as a native string."""
        return self._path

    def get_properties(self) -> dict[str, object]:
        """Return configured advertisement properties as native Python values."""
        properties: dict[str, object] = {
            "Type": self._advertisement_type.value,
            "Includes": self.includes,
        }
        if self._service_uuids:
            properties["ServiceUUIDs"] = self.service_uuids
        if self._manufacturer_data:
            properties["ManufacturerData"] = self.manufacturer_data_values
        if self._service_data:
            properties["ServiceData"] = self.service_data
        if self._local_name is not None:
            properties["LocalName"] = self._local_name
        if self._appearance is not None:
            properties["Appearance"] = self._appearance
        return properties

    def release(self) -> None:
        """Mark the advertisement released and invoke its cleanup callback."""
        if self._released:
            return
        self._released = True
        if self.on_release is not None:
            self.on_release()

    async def _release_async(self) -> None:
        """Handle a D-Bus release."""
        if self._released:
            return
        self._released = True
        if self.on_release is not None:
            self.on_release()

    def _bind_adapter(self, adapter: Adapter) -> None:
        """Bind this advertisement to an initialized adapter manager."""
        self._adapter = adapter
        self._bus = adapter._bus
        self._manager = adapter._proxy.get_interface(LE_ADVERTISING_MANAGER_IFACE)

    def register(
        self,
        adapter: Adapter | None = None,
        options: Mapping[str, object] | None = None,
    ) -> None:
        """Register this advertisement on a local adapter."""
        if self._registered:
            return
        bound_adapter = adapter or self._adapter or Adapter()
        self._bind_adapter(bound_adapter)
        if not bound_adapter.discoverable:
            bound_adapter.discoverable = True
        _runtime.run(self._register_async(dict(options or {})))
        self._registered = True

    async def _register_async(
        self,
        options: dict[str, object],
    ) -> None:
        """Export and register this advertisement on the runtime loop."""
        self._bus.export(self._path, self._interface)
        try:
            await cast(Any, self._manager).call_register_advertisement(
                self._path,
                _dbus.pack_options(options),
            )
        except Exception:
            self._bus.unexport(self._path, self._interface)
            raise

    def unregister(self) -> None:
        """Unregister this advertisement."""
        if not self._registered:
            return
        _runtime.run(self._unregister_async())
        self._registered = False

    async def _unregister_async(self) -> None:
        """Unregister and unexport this advertisement on the runtime loop."""
        try:
            await cast(Any, self._manager).call_unregister_advertisement(self._path)
        except DBusError as error:
            if "DoesNotExist" not in str(error.type):
                raise
        self._bus.unexport(self._path, self._interface)


__all__ = ["Advertisement"]
