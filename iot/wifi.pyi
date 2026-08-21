# ruff: noqa: E402
"""Wi-Fi station and access-point management."""

import os
import socket
from contextlib import AbstractContextManager

_WIFI_INIT_SCRIPT = ...
_WIFI_INTERFACE = ...
_WIFI_SETTINGS = ...
_WIFI_SETTINGS_FALLBACK = ...
class WifiError(OSError):
    """Raised when a Wi-Fi operation cannot be completed."""
    ...


class WifiScanResult:
    """A wireless network reported by a Wi-Fi scan."""
    bssid: str
    """Basic service set identifier (access-point MAC address)."""
    frequency: int
    """Center frequency in megahertz."""
    channel: int
    """Wifi channel"""
    signal_level: int
    """Received signal level in dBm."""
    flags: str
    """Authentication, encryption, and capability flags."""
    ssid: str
    """Service set identifier."""
    def __init__(self, bssid: str, frequency: int, channel: int, signal_level: int, flags: str, ssid: str) -> None:
        """Initialize a wireless scan result."""
        ...



class Wifi:
    """Manage wlan0 in station or access-point mode."""
    control_path: str
    """Directory containing wpa_supplicant interface control sockets."""
    timeout: float
    """Maximum number of seconds to wait for a daemon response."""
    @staticmethod
    def _parse_scan_results(response: str) -> list[WifiScanResult]: ...
    def _control_socket(self) -> AbstractContextManager[socket.socket]: ...
    def __init__(self, *, control_path: str | os.PathLike[str] = ..., timeout: float = ...) -> None:
        """Configure access to the wlan0 wpa_supplicant control socket."""
        ...

    def scan(self) -> list[WifiScanResult]:
        """Scan for nearby networks and return their advertised details."""
        ...

    def connect(self, ssid: str, password: str = ..., *, wait: bool = ..., timeout: float = ...) -> int:
        """Configure and select a network, returning its wpa_supplicant ID.

        Omit ``password`` for an open network. WPA/WPA2 passphrases must be
        8 to 63 characters; a 64-character hexadecimal pre-shared key is also
        accepted. By default, wait up to ``timeout`` seconds for the selected
        network to connect. Set ``wait`` to ``False`` to return immediately
        after selecting it.
        """
        ...

    def start_default(self) -> None:
        """Restart Wi-Fi using the installed default settings."""
        ...

    def start_sta(self) -> None:
        """Start station mode on wlan0 using the installed Wi-Fi settings."""
        ...

    def start_ap(self, ssid: str, password: str = ..., *, address: str = ..., prefix: int = ..., channel: int = ..., country: str = ..., hidden: bool = ...) -> None:
        """Start an access point on wlan0 using the supplied settings."""
        ...

    def disconnect(self) -> None:
        """Disconnect the Wi-Fi interface from its current network."""
        ...

    def is_connected(self) -> bool:
        """Return whether the interface has completed a Wi-Fi connection."""
        ...

    def get_ip(self) -> str | None:
        """Return the current IP address, or ``None`` when unavailable."""
        ...

    def get_mac(self) -> str | None:
        """Return the interface MAC address, or ``None`` when unavailable."""
        ...

    def get_status(self) -> str:
        """Return the active Wi-Fi mode reported by the service."""
        ...

    def status(self) -> dict[str, str]:
        """Return the current wpa_supplicant status fields."""
        ...
    @staticmethod
    def _validate_ssid(ssid: str) -> None: ...
    @staticmethod
    def _validate_psk(password: str) -> None: ...
    def _find_network(self, ssid: str) -> int | None: ...
    def _wait_for_connection(self, network_id: int, timeout: float) -> None: ...
    @staticmethod
    def _switch_mode(config: str, error_message: str) -> None: ...
    @staticmethod
    def _command(control: socket.socket, command: str) -> str: ...
    @staticmethod
    def _command_ok(control: socket.socket, command: str) -> None: ...
    def _request(self, command: str) -> str: ...
    def _request_ok(self, command: str) -> None: ...
