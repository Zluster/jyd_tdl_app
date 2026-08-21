"""Convenience wrapper for publishing Bluetooth Low Energy peripherals."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from typing import Any, cast

from dbus_fast.errors import DBusError

from . import _dbus, _runtime
from ._constants import GATT_MANAGER_IFACE
from .adapter import Adapter
from .advertisement import Advertisement
from .enums import AdvertisementType, CharacteristicFlag, DescriptorFlag
from .server import Application, LocalCharacteristic, LocalDescriptor, LocalService


_ByteData = bytes | bytearray | memoryview | Sequence[int]


class Peripheral:
    """Build and publish a connectable local Bluetooth GATT peripheral."""

    application: Application
    """Local GATT application owned by this peripheral."""
    adapter: Adapter
    """Local Bluetooth adapter used by this peripheral."""
    advertisement: Advertisement
    """Connectable advertisement published with the GATT application."""

    def __init__(
        self,
        adapter_address: str | None = None,
        local_name: str | None = None,
        appearance: int | None = None,
    ) -> None:
        """Create an unpublished peripheral on a selected local adapter."""
        self.application = Application()
        self.adapter = Adapter(adapter_address)
        self._manager = self.adapter._proxy.get_interface(GATT_MANAGER_IFACE)
        self.advertisement = Advertisement(1, AdvertisementType.PERIPHERAL)
        self.advertisement._bind_adapter(self.adapter)
        self.advertisement.local_name = local_name
        self.advertisement.appearance = appearance
        self._services: list[LocalService] = []
        self._characteristics: list[LocalCharacteristic] = []
        self._descriptors: list[LocalDescriptor] = []
        self._primary_services: list[str] = []
        self._published = False

    def add_service(
        self,
        service_id: int,
        uuid: str,
        primary: bool = True,
    ) -> LocalService:
        """Add and return a local GATT service definition."""
        service = LocalService(service_id, uuid, primary)
        self._services.append(service)
        self.application.add_managed_object(service)
        if primary:
            self._primary_services.append(uuid)
        return service

    def add_characteristic(
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
    ) -> LocalCharacteristic:
        """Add and return a local GATT characteristic definition."""
        characteristic = LocalCharacteristic(
            service_id,
            characteristic_id,
            uuid,
            value,
            notifying,
            flags,
            read_callback,
            write_callback,
            notify_callback,
        )
        self._characteristics.append(characteristic)
        self.application.add_managed_object(characteristic)
        return characteristic

    def add_descriptor(
        self,
        service_id: int,
        characteristic_id: int,
        descriptor_id: int,
        uuid: str,
        value: _ByteData = b"",
        flags: Sequence[DescriptorFlag] = (),
    ) -> LocalDescriptor:
        """Add and return a local GATT descriptor definition."""
        descriptor = LocalDescriptor(
            service_id,
            characteristic_id,
            descriptor_id,
            uuid,
            value,
            flags,
        )
        self._descriptors.append(descriptor)
        self.application.add_managed_object(descriptor)
        return descriptor

    @property
    def on_connect(self) -> Callable[..., Any] | None:
        """Return the adapter callback invoked for remote connections."""
        return self.adapter.on_connect

    @on_connect.setter
    def on_connect(self, callback: Callable[..., Any] | None) -> None:
        """Set the adapter callback invoked for remote connections."""
        self.adapter.on_connect = callback

    @property
    def on_disconnect(self) -> Callable[..., Any] | None:
        """Return the adapter callback invoked for remote disconnections."""
        return self.adapter.on_disconnect

    @on_disconnect.setter
    def on_disconnect(self, callback: Callable[..., Any] | None) -> None:
        """Set the adapter callback invoked for remote disconnections."""
        self.adapter.on_disconnect = callback

    @property
    def published(self) -> bool:
        """Return whether the peripheral is currently registered with BlueZ."""
        return self._published

    def publish(self) -> None:
        """Register the GATT application and advertisement, then return."""
        if self._published:
            return
        self.advertisement.service_uuids = self._primary_services
        if not self.adapter.powered:
            self.adapter.powered = True
        self.application.export()
        _runtime.run(self._register_application_async())
        self.advertisement.register(self.adapter)
        self._published = True

    async def _register_application_async(self) -> None:
        """Register this application's root with BlueZ on the runtime loop."""
        await cast(Any, self._manager).call_register_application(
            self.application.get_path(),
            _dbus.pack_options({}),
        )

    def unpublish(self) -> None:
        """Unregister the advertisement and GATT application."""
        if not self._published:
            return
        self.advertisement.unregister()
        _runtime.run(self._unregister_application_async())
        self.application.unexport()
        self._published = False

    async def _unregister_application_async(self) -> None:
        """Unregister this application's root from BlueZ on the runtime loop."""
        try:
            await cast(Any, self._manager).call_unregister_application(
                self.application.get_path()
            )
        except DBusError as error:
            if "DoesNotExist" not in str(error.type):
                raise

    def stop(self) -> None:
        """Alias for :meth:`unpublish`."""
        self.unpublish()


__all__ = ["Peripheral"]
