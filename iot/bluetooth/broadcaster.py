"""Non-connectable Bluetooth LE beacon broadcasting convenience wrapper."""

from __future__ import annotations

from collections.abc import Sequence

from .adapter import Adapter
from .advertisement import Advertisement
from .enums import AdvertisementType


_ByteData = bytes | bytearray | memoryview | Sequence[int]


class Beacon:
    """Configure and publish a non-connectable Bluetooth LE advertisement."""

    adapter: Adapter
    """Local Bluetooth adapter used to broadcast this beacon."""
    advertisement: Advertisement
    """Broadcast advertisement containing this beacon's data."""

    def __init__(
        self,
        adapter_addr: str | None = None,
        *,
        advert_id: int = 1,
    ) -> None:
        """Create an inactive beacon on a selected local adapter."""
        self.adapter = Adapter(adapter_addr)
        self.advertisement = Advertisement(advert_id, AdvertisementType.BROADCAST)
        self.advertisement._bind_adapter(self.adapter)
        self._started = False

    def add_service_data(self, service: str, data: _ByteData) -> None:
        """Add or replace payload data for an advertised service UUID."""
        service_data = self.advertisement.service_data
        service_data[service] = bytes(data)
        self.advertisement.service_data = service_data
        service_uuids = self.advertisement.service_uuids
        if service not in service_uuids:
            service_uuids.append(service)
            self.advertisement.service_uuids = service_uuids

    def add_manufacturer_data(
        self,
        manufacturer: int | str,
        data: _ByteData,
    ) -> None:
        """Add manufacturer data using an integer or hexadecimal identifier."""
        if isinstance(manufacturer, str):
            manufacturer = int(manufacturer, 16)
        self.advertisement.manufacturer_data(manufacturer, data)

    def include_tx_power(self, state: bool | None = None) -> bool:
        """Get or set whether the controller includes transmit power."""
        if state is not None:
            self.advertisement.include_tx_power = state
        return self.advertisement.include_tx_power

    @property
    def started(self) -> bool:
        """Return whether the beacon advertisement is registered."""
        return self._started

    def start_beacon(self) -> None:
        """Register the beacon advertisement and return immediately."""
        if not self.adapter.powered:
            self.adapter.powered = True
        self.advertisement.register(self.adapter)
        self._started = True

    def stop_beacon(self) -> None:
        """Unregister the beacon advertisement."""
        self.advertisement.unregister()
        self._started = False


__all__ = ["Beacon"]
