"""Private helpers for BlueZ proxy lookup and D-Bus value conversion."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import re
from typing import Any, TypeAlias, cast

from dbus_fast import Variant
from dbus_fast.aio import MessageBus, ProxyInterface, ProxyObject
from dbus_fast.errors import DBusError

from ._constants import (
    ADAPTER_INTERFACE,
    BLUEZ_SERVICE_NAME,
    DEVICE_INTERFACE,
    GATT_CHRC_IFACE,
    GATT_DESC_IFACE,
    GATT_SERVICE_IFACE,
)
ManagedObjects: TypeAlias = dict[str, dict[str, dict[str, Any]]]
ByteData: TypeAlias = bytes | bytearray | memoryview | Sequence[int]

_DEVICE_PATH = re.compile(
    r"(?:^|/)dev_((?:[0-9A-Fa-f]{2}_){5}[0-9A-Fa-f]{2})(?:/|$)"
)
_ADAPTER_PATH = re.compile(r"^(/org/bluez/hci\d+)(?:/|$)")
_node_cache: dict[tuple[MessageBus, str], Any] = {}

_OPTION_SIGNATURES = {
    "offset": "q",
    "mtu": "q",
    "device": "o",
    "link": "s",
    "type": "s",
    "prepare-authorize": "b",
    "UUIDs": "as",
    "RSSI": "n",
    "Pathloss": "q",
    "Transport": "s",
    "DuplicateData": "b",
    "Discoverable": "b",
    "Pattern": "s",
}


def unwrap(value: Any) -> Any:
    """Recursively convert D-Bus variants into plain Python values."""
    if isinstance(value, Variant):
        if value.signature == "ay":
            return bytes(value.value)
        return unwrap(value.value)
    if isinstance(value, Mapping):
        return {unwrap(key): unwrap(item) for key, item in value.items()}
    if isinstance(value, list):
        return [unwrap(item) for item in value]
    if isinstance(value, tuple):
        return tuple(unwrap(item) for item in value)
    if isinstance(value, (bytearray, memoryview)):
        return bytes(value)
    return value


def pack_bytes(data: ByteData) -> bytes:
    """Normalize bytes or integer byte sequences for an ``ay`` argument."""
    return bytes(data)


def _infer_signature(value: object) -> str:
    """Infer a D-Bus signature for ordinary option values."""
    if isinstance(value, bool):
        return "b"
    if isinstance(value, str):
        return "s"
    if isinstance(value, int):
        return "i"
    if isinstance(value, float):
        return "d"
    if isinstance(value, (bytes, bytearray, memoryview)):
        return "ay"
    if isinstance(value, Mapping):
        return "a{sv}"
    if isinstance(value, Sequence):
        if all(isinstance(item, str) for item in value):
            return "as"
        if all(isinstance(item, int) for item in value):
            return "ay"
    raise TypeError(f"cannot infer a D-Bus signature for {type(value).__name__}")


def _normalize_for_signature(value: object, signature: str) -> object:
    """Normalize a Python value for construction of a D-Bus variant."""
    if signature == "ay":
        return pack_bytes(cast(ByteData, value))
    if signature == "as":
        return list(cast(Sequence[str], value))
    if signature == "a{sv}":
        return pack_options(cast(Mapping[str, object], value))
    return value


def pack_options(
    options: Mapping[str, object],
    *,
    signatures: Mapping[str, str] | None = None,
) -> dict[str, Variant]:
    """Pack a plain option mapping as the variants required by ``a{sv}``."""
    overrides = signatures or {}
    packed: dict[str, Variant] = {}
    for name, value in options.items():
        if isinstance(value, Variant):
            packed[name] = value
            continue
        signature = overrides.get(name, _OPTION_SIGNATURES.get(name))
        if signature is None:
            signature = _infer_signature(value)
        packed[name] = Variant(signature, _normalize_for_signature(value, signature))
    return packed


async def get_proxy(bus: MessageBus, path: str) -> ProxyObject:
    """Return a BlueZ proxy, reusing introspection data for the bus and path."""
    key = (bus, path)
    node = _node_cache.get(key)
    if node is None:
        node = await bus.introspect(BLUEZ_SERVICE_NAME, path)
        _node_cache[key] = node
    return bus.get_proxy_object(BLUEZ_SERVICE_NAME, path, node)


async def get_managed_objects(manager: ProxyInterface) -> ManagedObjects:
    """Return the BlueZ object tree using only native Python values."""
    managed = await cast(Any, manager).call_get_managed_objects()
    return cast(ManagedObjects, unwrap(managed))


async def get_prop(
    manager: ProxyInterface,
    interface: str,
    name: str,
    default: Any = None,
) -> Any:
    """Read a property, returning ``default`` when BlueZ omits it."""
    try:
        value = await cast(Any, manager).call_get(interface, name)
    except DBusError as error:
        if (
            "UnknownProperty" in str(error.type)
            or "no such property" in error.text.casefold()
        ):
            return default
        raise
    return unwrap(value)


async def get_all_props(
    manager: ProxyInterface,
    interface: str,
) -> dict[str, Any]:
    """Return every property on an interface as native Python values."""
    values = await cast(Any, manager).call_get_all(interface)
    return cast(dict[str, Any], unwrap(values))


async def set_prop(
    manager: ProxyInterface,
    interface: str,
    name: str,
    value: object,
    signature: str | None = None,
) -> None:
    """Set a property after packing its value into a private variant."""
    value_signature = signature or _infer_signature(value)
    variant = Variant(
        value_signature,
        _normalize_for_signature(value, value_signature),
    )
    await cast(Any, manager).call_set(interface, name, variant)


def address_from_path(path: str) -> str:
    """Return a remote-device address encoded in a BlueZ object path."""
    match = _DEVICE_PATH.search(path)
    return match.group(1).replace("_", ":").upper() if match else ""


def adapter_address_from_path(
    path: str,
    managed: Mapping[str, Mapping[str, Mapping[str, Any]]] | None = None,
) -> str:
    """Return the adapter address owning a BlueZ object path."""
    if managed is None:
        return ""
    match = _ADAPTER_PATH.match(path)
    if match is None:
        return ""
    adapter = managed.get(match.group(1), {}).get(ADAPTER_INTERFACE, {})
    value = unwrap(adapter.get("Address", ""))
    return value if isinstance(value, str) else ""


def find_object_path(
    managed: Mapping[str, Mapping[str, Mapping[str, Any]]],
    *,
    adapter: str | None = None,
    device: str | None = None,
    service: str | None = None,
    characteristic: str | None = None,
    descriptor: str | None = None,
) -> str | None:
    """Resolve an adapter, device, service, characteristic, or descriptor path."""
    selectors = (
        (adapter, ADAPTER_INTERFACE, "Address"),
        (device, DEVICE_INTERFACE, "Address"),
        (service, GATT_SERVICE_IFACE, "UUID"),
        (characteristic, GATT_CHRC_IFACE, "UUID"),
        (descriptor, GATT_DESC_IFACE, "UUID"),
    )
    path: str | None = None
    for expected, interface, property_name in selectors:
        if expected is None:
            continue
        path = next(
            (
                candidate
                for candidate, interfaces in managed.items()
                if (properties := interfaces.get(interface)) is not None
                and (path is None or candidate.startswith(f"{path}/"))
                and str(properties.get(property_name, "")).casefold()
                == expected.casefold()
            ),
            None,
        )
        if path is None:
            return None
    return path
