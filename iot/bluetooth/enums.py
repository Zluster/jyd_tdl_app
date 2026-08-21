"""Dara-owned enumerations for Bluetooth wire values."""

from enum import Enum


class AdvertisementType(str, Enum):
    """Bluetooth LE advertisement role."""

    PERIPHERAL = "peripheral"
    BROADCAST = "broadcast"


class CharacteristicFlag(str, Enum):
    """Capability or security requirement of a GATT characteristic."""

    BROADCAST = "broadcast"
    READ = "read"
    WRITE_WITHOUT_RESPONSE = "write-without-response"
    WRITE = "write"
    NOTIFY = "notify"
    INDICATE = "indicate"
    AUTHENTICATED_SIGNED_WRITES = "authenticated-signed-writes"
    EXTENDED_PROPERTIES = "extended-properties"
    RELIABLE_WRITE = "reliable-write"
    WRITABLE_AUXILIARIES = "writable-auxiliaries"
    ENCRYPT_READ = "encrypt-read"
    ENCRYPT_WRITE = "encrypt-write"
    ENCRYPT_AUTHENTICATED_READ = "encrypt-authenticated-read"
    ENCRYPT_AUTHENTICATED_WRITE = "encrypt-authenticated-write"
    SECURE_READ = "secure-read"
    SECURE_WRITE = "secure-write"
    AUTHORIZE = "authorize"


class DescriptorFlag(str, Enum):
    """Capability or security requirement of a GATT descriptor."""

    READ = "read"
    WRITE = "write"
    ENCRYPT_READ = "encrypt-read"
    ENCRYPT_WRITE = "encrypt-write"
    ENCRYPT_AUTHENTICATED_READ = "encrypt-authenticated-read"
    ENCRYPT_AUTHENTICATED_WRITE = "encrypt-authenticated-write"
    SECURE_READ = "secure-read"
    SECURE_WRITE = "secure-write"
    AUTHORIZE = "authorize"
