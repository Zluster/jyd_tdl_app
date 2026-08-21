"""Dara BLE stack built on the BlueZ D-Bus API."""

from .errors import (
    AdapterNotFoundError,
    BluetoothError,
    BluetoothTimeoutError,
    DeviceNotFoundError,
    GattError,
)
from .adapter import Adapter, list_adapters
from .advertisement import Advertisement
from .broadcaster import Beacon
from .central import Central
from .device import Device
from .enums import AdvertisementType, CharacteristicFlag, DescriptorFlag
from .gatt import RemoteCharacteristic, RemoteDescriptor, RemoteService
from .peripheral import Peripheral
from .observer import (
    AltBeacon,
    EddystoneTLM,
    EddystoneUID,
    EddystoneURL,
    IBeacon,
    Scanner,
    scan_eddystone,
)
from .server import Application, LocalCharacteristic, LocalDescriptor, LocalService


__all__ = [
    "AdapterNotFoundError",
    "Adapter",
    "Advertisement",
    "AltBeacon",
    "Application",
    "AdvertisementType",
    "BluetoothError",
    "BluetoothTimeoutError",
    "Beacon",
    "Central",
    "CharacteristicFlag",
    "DescriptorFlag",
    "Device",
    "DeviceNotFoundError",
    "EddystoneTLM",
    "EddystoneUID",
    "EddystoneURL",
    "GattError",
    "IBeacon",
    "LocalCharacteristic",
    "LocalDescriptor",
    "LocalService",
    "Peripheral",
    "RemoteCharacteristic",
    "RemoteDescriptor",
    "RemoteService",
    "Scanner",
    "list_adapters",
    "scan_eddystone",
]
