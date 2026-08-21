"""Dara-owned exceptions raised by Bluetooth operations."""


class BluetoothError(OSError):
    """Raised when a Bluetooth operation cannot be completed."""


class BluetoothTimeoutError(BluetoothError):
    """Raised when a Bluetooth operation exceeds its allowed time."""


class AdapterNotFoundError(BluetoothError):
    """Raised when a requested Bluetooth adapter cannot be found."""


class DeviceNotFoundError(BluetoothError):
    """Raised when a requested Bluetooth device cannot be found."""


class GattError(BluetoothError):
    """Raised when a GATT object cannot be resolved or accessed."""
