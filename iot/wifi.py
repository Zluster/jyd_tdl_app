"""Wi-Fi station and access-point management."""


from contextlib import contextmanager
from ipaddress import IPv4Interface
import os
import shlex
import socket
import subprocess
from tempfile import NamedTemporaryFile, TemporaryDirectory
from time import monotonic, sleep

from dara.core._error_helpers import wrap_error_as


_WIFI_INIT_SCRIPT = "/etc/init.d/S31wifi"
_WIFI_INTERFACE = "wlan0"
_WIFI_SETTINGS = "/data/etc/jyd/wifi.conf"
_WIFI_SETTINGS_FALLBACK = "/etc/jyd/wifi.conf"


class WifiError(OSError):
    """Raised when a Wi-Fi operation cannot be completed."""


class WifiScanResult:
    """A wireless network reported by a Wi-Fi scan."""

    def __init__(
        self,
        bssid,
        frequency,
        channel,
        signal_level,
        flags,
        ssid,
    ):
        """Initialize a wireless scan result."""
        self.bssid = bssid
        self.frequency = frequency
        self.channel = channel
        self.signal_level = signal_level
        self.flags = flags
        self.ssid = ssid


class Wifi:
    """Manage wlan0 in station or access-point mode."""

    """Directory containing wpa_supplicant interface control sockets."""
    """Maximum number of seconds to wait for a daemon response."""

    def __init__(
        self,
        *,
        control_path = "/var/run/wpa_supplicant",
        timeout = 10.0,
    ):
        """Configure access to the wlan0 wpa_supplicant control socket."""
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.control_path = os.fspath(control_path)
        self.timeout = timeout

    # we scan, we connect, we... ping?

#region scan

    @wrap_error_as(WifiError, "Wi-Fi scan failed", catch=(OSError, UnicodeError))
    def scan(self):
        """Scan for nearby networks and return their advertised details."""
        with self._control_socket() as control:
            self._command_ok(control, "ATTACH")
            deadline = monotonic() + self.timeout
            scan_finished = False
            scan_started = False
            control.send(b"SCAN")
            while not scan_finished or not scan_started:
                control.settimeout(max(0.0, deadline - monotonic()))
                response = control.recv(65535).decode().rstrip("\n")
                if "CTRL-EVENT-SCAN-RESULTS" in response:
                    scan_finished = True
                elif "CTRL-EVENT-SCAN-FAILED" in response:
                    raise WifiError("Wi-Fi scan failed")
                elif not response.startswith("<"):
                    if response != "OK":
                        raise WifiError(f"Wi-Fi scan failed: {response}")
                    scan_started = True
            return self._parse_scan_results(self._command(control, "SCAN_RESULTS"))

    @staticmethod
    def _parse_scan_results(response):
        """Convert a SCAN_RESULTS response into Dara-owned values."""
        results = []
        for line in response.splitlines()[1:]:
            fields = line.split("\t", 4)
            if len(fields) != 5:
                continue
            try:
                chn = 0
                if (int(fields[1]) >= 2412 and int(fields[1]) <= 2484):
                    chn = (int(fields[1]) - 2412) // 5 + 1
                elif (int(fields[1]) >= 5170 and int(fields[1]) <= 5825):
                    chn = (int(fields[1]) - 5170) // 5 + 34
                results.append(
                    WifiScanResult(
                        bssid=fields[0],
                        frequency=int(fields[1]),
                        channel=chn,
                        signal_level=int(fields[2]),
                        flags=fields[3],
                        ssid=fields[4],
                    )
                )
            except ValueError:
                continue
        return results

#endregion scan

#region connect

    def connect(
        self,
        ssid,
        password = "",
        *,
        wait = True,
        timeout = 20.0,
    ):
        """Configure and select a network, returning its wpa_supplicant ID.

        Omit ``password`` for an open network. WPA/WPA2 passphrases must be
        8 to 63 characters; a 64-character hexadecimal pre-shared key is also
        accepted. By default, wait up to ``timeout`` seconds for the selected
        network to connect. Set ``wait`` to ``False`` to return immediately
        after selecting it.
        """
        self._validate_ssid(ssid)
        self._validate_psk(password)
        if wait and timeout <= 0:
            raise ValueError("timeout must be positive when wait is enabled")
        network_id = self._find_network(ssid)
        created = network_id is None
        if network_id is None:
            try:
                network_id = int(self._request("ADD_NETWORK"))
            except ValueError as error:
                raise WifiError("wpa_supplicant did not create a network") from error

        try:
            # yes, this is safe, wpa_supplicant directly copies content between the first " and the last "
            self._request_ok(f'SET_NETWORK {network_id} ssid "{ssid}"')
            if password: # not empty
                self._request_ok(f'SET_NETWORK {network_id} key_mgmt WPA-PSK')
                # yes, this is safe, wpa_supplicant directly copies content between the first " and the last "
                self._request_ok(f'SET_NETWORK {network_id} psk "{password}"')
            else:
                self._request_ok(f'SET_NETWORK {network_id} key_mgmt NONE')
                self._request_ok(f'SET_NETWORK {network_id} psk ""')
            self._request_ok(f"SELECT_NETWORK {network_id}")
            self._request_ok(f"ENABLE_NETWORK {network_id}")
            if wait:
                self._wait_for_connection(network_id, timeout)
        except Exception:
            if created:
                try:
                    self._request(f"REMOVE_NETWORK {network_id}")
                except WifiError:
                    pass
            raise
        return network_id
    
    @staticmethod
    def _validate_ssid(ssid):
        """Validate an SSID before sending it to wpa_supplicant."""
        if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32:
            raise ValueError("ssid must contain 1 to 32 UTF-8 bytes")
        if any(character in ssid for character in ("\x00", "\n", "\r")):
            raise ValueError("ssid contains an unsupported control character")

    @staticmethod
    def _validate_psk(password):
        """Validate a hostapd passphrase, allowing an empty open-network value."""
        if not isinstance(password, str):
            raise ValueError("password must be a string")
        if not password:
            return
        if not 8 <= len(password) <= 63 or not password.isascii() or not password.isprintable():
            raise ValueError("password must contain 8 to 63 printable ASCII characters")

    def _find_network(self, ssid):
        """Return the ID of an existing network with the requested SSID."""
        # handle edge case where ssid contains '\t'
        expected_ssid = f'"{ssid}"'
        for line in self._request("LIST_NETWORKS").splitlines()[1:]:
            try:
                network_id = int(line.split("\t", 1)[0])
            except ValueError:
                continue
            if self._request(f"GET_NETWORK {network_id} ssid") == expected_ssid:
                return network_id
        return None

    def _wait_for_connection(self, network_id, timeout):
        """Wait until the selected network completes its connection."""
        deadline = monotonic() + timeout
        while True:
            status = self.status()
            if status.get("wpa_state") == "COMPLETED" and status.get("id") == str(
                network_id
            ):
                return
            remaining = deadline - monotonic()
            if remaining <= 0:
                raise WifiError(
                    f"Wi-Fi connection timed out after {timeout:g} seconds"
                )
            sleep(min(0.1, remaining))

#endregion connect

#region mode switching

    @wrap_error_as(
        WifiError,
        "Wi-Fi default start failed",
        catch=(OSError, subprocess.SubprocessError),
    )
    def start_default(self):
        """Restart Wi-Fi using the installed default settings."""
        subprocess.run([_WIFI_INIT_SCRIPT, "restart"], check=True)

    @wrap_error_as(
        WifiError,
        "Wi-Fi station start failed",
        catch=(OSError, subprocess.SubprocessError),
    )
    def start_sta(self):
        """Start station mode on wlan0 using the installed Wi-Fi settings."""
        # urhhh a bit evil but it works
        self._switch_mode(
            "\n".join(
                (
                    f"if [ -r {_WIFI_SETTINGS} ]; then",
                    f"    . {_WIFI_SETTINGS}",
                    f"elif [ -r {_WIFI_SETTINGS_FALLBACK} ]; then",
                    f"    . {_WIFI_SETTINGS_FALLBACK}",
                    "fi",
                    "WIFI_MODE=sta",
                    f"WIFI_INTERFACE={_WIFI_INTERFACE}",
                    "",
                )
            ),
            "Wi-Fi station start failed",
        )

    @wrap_error_as(
        WifiError,
        "Wi-Fi access point start failed",
        catch=(OSError, subprocess.SubprocessError),
    )
    def start_ap(
        self,
        ssid,
        password = "",
        *,
        address = "192.168.43.1",
        prefix = 24,
        channel = 6,
        country = "CN",
        hidden = False,
    ):
        """Start an access point on wlan0 using the supplied settings."""
        self._validate_ssid(ssid)
        self._validate_psk(password)
        if (
            isinstance(channel, bool)
            or not isinstance(channel, int)
            or not 1 <= channel <= 13
        ):
            raise ValueError("channel must be an integer from 1 to 13")
        if (
            not isinstance(country, str)
            or len(country) != 2
            or not country.isascii()
            or not country.isalpha()
        ):
            raise ValueError("country must contain two ASCII letters")
        if not isinstance(hidden, bool):
            raise ValueError("hidden must be a bool")
        if (
            not isinstance(address, str)
            or isinstance(prefix, bool)
            or not isinstance(prefix, int)
        ):
            raise ValueError("address and prefix must define an IPv4 interface")
        try:
            interface = IPv4Interface(f"{address}/{prefix}")
        except ValueError as error:
            raise ValueError("address and prefix must define an IPv4 interface") from error
        if interface.network.prefixlen > 30:
            raise ValueError("prefix must provide at least one client address")
        if interface.ip in (
            interface.network.network_address,
            interface.network.broadcast_address,
        ):
            raise ValueError("address must be a usable host address")

        first = interface.network.network_address + 1
        last = interface.network.broadcast_address - 1
        if interface.ip < last:
            dhcp_start, dhcp_end = interface.ip + 1, last
        else:
            dhcp_start, dhcp_end = first, interface.ip - 1
        settings = {
            "WIFI_MODE": "ap",
            "WIFI_INTERFACE": _WIFI_INTERFACE,
            "AP_SSID": ssid,
            "AP_PASSWORD": password,
            "AP_ADDRESS": str(interface.ip),
            "AP_PREFIX": str(interface.network.prefixlen),
            "AP_CHANNEL": str(channel),
            "AP_COUNTRY": country.upper(),
            "AP_HIDDEN": str(int(hidden)),
            "AP_DHCP_START": str(dhcp_start),
            "AP_DHCP_END": str(dhcp_end),
            "AP_DHCP_LEASE": "12h",
        }
        self._switch_mode(
            "".join(
                f"{name}={shlex.quote(value)}\n" for name, value in settings.items()
            ),
            "Wi-Fi access point start failed",
        )

    @staticmethod
    def _switch_mode(config, error_message):
        """Restart Wi-Fi with temporary settings and restore defaults on failure."""
        with NamedTemporaryFile(
            "w", encoding="utf-8", prefix="dara-wifi-", suffix=".conf"
        ) as settings:
            settings.write(config)
            settings.flush()
            try:
                subprocess.run(
                    [_WIFI_INIT_SCRIPT, "restart", settings.name], check=True
                )
            except (OSError, subprocess.SubprocessError) as error:
                try:
                    subprocess.run([_WIFI_INIT_SCRIPT, "restart"], check=True)
                except (OSError, subprocess.SubprocessError) as restore_error:
                    raise WifiError(
                        f"{error_message}; default Wi-Fi restoration failed"
                    ) from restore_error
                raise WifiError(error_message) from error

#endregion mode switching

    def disconnect(self):
        """Disconnect the Wi-Fi interface from its current network."""
        self._request_ok("DISCONNECT")

    def is_connected(self):
        """Return whether the interface has completed a Wi-Fi connection."""
        return self.status().get("wpa_state") == "COMPLETED"

    def get_ip(self):
        """Return the current IP address, or ``None`` when unavailable."""
        return self.status().get("ip_address")

    def get_mac(self):
        """Return the interface MAC address, or ``None`` when unavailable."""
        return self.status().get("address")

    @wrap_error_as(
        WifiError,
        "Wi-Fi status failed",
        catch=(OSError, subprocess.SubprocessError, UnicodeError),
    )
    def get_status(self):
        """Return the active Wi-Fi mode reported by the service."""
        return subprocess.run(
            [_WIFI_INIT_SCRIPT, "status"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    
    def status(self):
        """Return the current wpa_supplicant status fields."""
        return dict(
            line.split("=", 1)
            for line in self._request("STATUS").splitlines()
            if "=" in line
        )

    # def save_networks(self) -> None:
    #     """Persist configured networks using wpa_supplicant's configuration."""
    #     self._request_ok("SAVE_CONFIG")

#region some helpers

    @contextmanager
    def _control_socket(self):
        """Yield a connected local wpa_supplicant control socket."""
        with TemporaryDirectory(prefix="dara-wifi-") as directory:
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as control:
                control.settimeout(self.timeout)
                control.bind(os.path.join(directory, "control"))
                control.connect(os.path.join(self.control_path, _WIFI_INTERFACE))
                yield control

    @staticmethod
    def _command(control, command):
        """Send a command and ignore unsolicited events before its reply."""
        control.send(command.encode())
        while True:
            response = control.recv(65535).decode()
            if not response.startswith("<"):
                return response.rstrip("\n")

    @staticmethod
    def _command_ok(control, command):
        """Raise ``WifiError`` unless a command response is successful."""
        response = Wifi._command(control, command)
        if response != "OK":
            raise WifiError(f"Wi-Fi service cmd ({command}) failed: {response}")

    @wrap_error_as(
        WifiError,
        "Wi-Fi command failed",
        catch=(OSError, UnicodeError),
    )
    def _request(self, command):
        """Send one command to the control socket and return its response."""
        with self._control_socket() as control:
            return self._command(control, command)

    @wrap_error_as(
        WifiError,
        "Wi-Fi command failed",
        catch=(OSError, UnicodeError),
    )
    def _request_ok(self, command):
        """Raise ``WifiError`` unless a command response is successful."""
        response = self._request(command)
        if response != "OK":
            raise WifiError(f"Wi-Fi service req ({command}) failed: {response}")

#endregion
