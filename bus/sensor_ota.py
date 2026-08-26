"""UART OTA client for cascaded GD32 nodes and direct bootloader recovery."""

from __future__ import annotations

import binascii
import errno
import os
import select
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from serial_port import SerialPort
from jydbus_uart import crc16_modbus

SENSOR_OTA_SLOT_A = 0
SENSOR_OTA_SLOT_B = 1
SENSOR_OTA_SLOT_AUTO = 0xFF

OTA_FRAME_REQUEST = 0x70
OTA_FRAME_RESPONSE = 0x71
OTA_CMD_STATUS = 0x01
OTA_CMD_BEGIN = 0x02
OTA_CMD_DATA = 0x03
OTA_CMD_FINISH = 0x04
OTA_CMD_ABORT = 0x05
OTA_CHUNK_SIZE = 128
OTA_SLOT_A_BASE = 0x08003800
OTA_SLOT_B_BASE = 0x08009C00
OTA_SLOT_SIZE = 0x00006400
OTA_MAX_FRAME = 265

RAW_MAGIC = 0x5AA55AA5
RAW_CMD_PING = 0x01
RAW_CMD_START = 0x02
RAW_CMD_DATA = 0x03
RAW_CMD_FINISH = 0x04
RAW_ACK = 0x80
RAW_HEADER_SIZE = 20

ProgressCallback = Callable[[int, int], None]


class OtaDeviceError(OSError):
    def __init__(self, status: int) -> None:
        self.status = status
        super().__init__(errno.EIO, f"OTA device rejected request: "
                         f"{sensor_ota_device_status_string(status)} ({status})")


@dataclass
class SensorOtaStatus:
    active_slot: int = SENSOR_OTA_SLOT_AUTO
    confirmed_slot: int = SENSOR_OTA_SLOT_AUTO
    pending_slot: int = SENSOR_OTA_SLOT_AUTO
    download_slot: int = SENSOR_OTA_SLOT_AUTO
    next_offset: int = 0
    image_size: int = 0
    image_crc32: int = 0
    image_version: int = 0
    slot_size: int = 0
    max_boot_attempts: int = 0


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def build_frame(frame_type: int, sensor_type: int, sensor_number: int,
                payload: bytes) -> bytes:
    if len(payload) > 0xFFFF:
        raise OSError(errno.EINVAL, "OTA payload is too large")
    body = struct.pack("<HBBB", len(payload), frame_type, sensor_type,
                       sensor_number) + payload
    return b"\x55" + body + struct.pack("<H", crc16_modbus(body)) + b"\xAA"


def validate_image(image: bytes, slot: int) -> None:
    if len(image) < 8 or len(image) > OTA_SLOT_SIZE:
        raise OSError(errno.EFBIG, "firmware size must be 8..25600 bytes")
    stack, reset = struct.unpack_from("<II", image)
    base = OTA_SLOT_A_BASE if slot == SENSOR_OTA_SLOT_A else OTA_SLOT_B_BASE
    if not 0x20000000 <= stack < 0x20002000:
        raise OSError(errno.EINVAL, "invalid firmware initial stack pointer")
    if reset & 1 == 0 or not base <= (reset & ~1) < base + OTA_SLOT_SIZE:
        raise OSError(errno.EINVAL, "image vector does not match slot link address")


class SensorOta:
    def __init__(self, device: str, baud_rate: int = 115200, debug: bool = False) -> None:
        if baud_rate not in (9600, 38400, 57600, 115200):
            raise OSError(errno.EINVAL, "unsupported OTA baud rate")
        self.port = SerialPort(device, baud_rate)
        self.debug = debug
        self.session = (int(time.time()) ^ os.getpid()) & 0xFFFFFFFF

    def close(self) -> None:
        self.port.close()

    def __enter__(self) -> "SensorOta":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def _debug_hex(self, direction: str, data: bytes) -> None:
        if self.debug:
            print(f"[OTA {direction}] " + " ".join(f"{value:02X}" for value in data),
                  file=sys.stderr)

    def _read_frame(self, timeout: float) -> bytes:
        frame = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            readable, _, _ = select.select([self.port.fd], [], [], max(remaining, 0.001))
            if not readable:
                break
            chunk = self.port.read_available(1)
            if not chunk:
                continue
            value = chunk[0]
            if not frame and value != 0x55:
                continue
            frame.append(value)
            if len(frame) >= 3 and struct.unpack_from("<H", frame, 1)[0] + 9 > OTA_MAX_FRAME:
                frame.clear()
                continue
            if len(frame) >= 3 and len(frame) == struct.unpack_from("<H", frame, 1)[0] + 9:
                if frame[-1] != 0xAA:
                    frame.clear()
                    continue
                received_crc = struct.unpack_from("<H", frame, len(frame) - 3)[0]
                if received_crc != crc16_modbus(frame[1:-3]):
                    frame.clear()
                    continue
                result = bytes(frame)
                self._debug_hex("RX", result)
                return result
        raise TimeoutError(errno.ETIMEDOUT, "OTA response timed out")

    def _transact(self, sensor_type: int, sensor_number: int, payload: bytes,
                  timeout: float) -> SensorOtaStatus:
        tx = build_frame(OTA_FRAME_REQUEST, sensor_type, sensor_number, payload)
        self._debug_hex("TX", tx)
        self.port.write_all(tx, drain=True)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            rx = self._read_frame(deadline - time.monotonic())
            payload_length = struct.unpack_from("<H", rx, 1)[0]
            if (rx[3] != OTA_FRAME_RESPONSE or rx[4] != sensor_type
                    or rx[5] != sensor_number or payload_length < 27):
                continue
            reply = rx[6:6 + payload_length]
            if reply[0] != payload[0]:
                continue
            status = SensorOtaStatus(
                active_slot=reply[2], confirmed_slot=reply[3], pending_slot=reply[4],
                download_slot=reply[5], next_offset=struct.unpack_from("<I", reply, 6)[0],
                image_size=struct.unpack_from("<I", reply, 10)[0],
                image_crc32=struct.unpack_from("<I", reply, 14)[0],
                image_version=struct.unpack_from("<I", reply, 18)[0],
                slot_size=struct.unpack_from("<I", reply, 22)[0],
                max_boot_attempts=reply[26])
            device_status = reply[1]
            if self.debug:
                print(f"[OTA ACK] cmd=0x{reply[0]:02X} status={device_status} "
                      f"({sensor_ota_device_status_string(device_status)}) "
                      f"next={status.next_offset}", file=sys.stderr)
            if device_status:
                raise OtaDeviceError(device_status)
            return status
        raise TimeoutError(errno.ETIMEDOUT, "OTA response timed out")

    def get_status(self, sensor_type: int, sensor_number: int) -> SensorOtaStatus:
        if sensor_number == 0:
            raise OSError(errno.EINVAL, "sensor number must be nonzero")
        return self._transact(sensor_type, sensor_number, bytes((OTA_CMD_STATUS,)), 2.0)

    def upgrade_file(self, sensor_type: int, sensor_number: int, target_slot: int,
                     image_path: str | os.PathLike[str], version: int = 1,
                     stop_after: int = 0,
                     progress: ProgressCallback | None = None) -> None:
        if target_slot not in (SENSOR_OTA_SLOT_A, SENSOR_OTA_SLOT_B):
            raise OSError(errno.EINVAL, "target slot must be A or B")
        image = Path(image_path).read_bytes()
        validate_image(image, target_slot)
        image_crc = crc32(image)
        if self.debug:
            print(f"[OTA IMAGE] file={image_path} slot={'A' if target_slot == 0 else 'B'} "
                  f"size={len(image)} crc32=0x{image_crc:08X} version={version}",
                  file=sys.stderr)
        self.session = (self.session + 1) & 0xFFFFFFFF
        begin = struct.pack("<BBIIII", OTA_CMD_BEGIN, target_slot, len(image),
                            image_crc, version & 0xFFFFFFFF, self.session)
        self._transact(sensor_type, sensor_number, begin, 10.0)
        offset = 0
        while offset < len(image):
            chunk = image[offset:offset + OTA_CHUNK_SIZE]
            if stop_after and offset >= stop_after:
                print(f"[OTA TEST] stopped intentionally at {offset}/{len(image)} bytes.",
                      file=sys.stderr)
                raise OSError(errno.ECANCELED, "OTA transfer stopped intentionally")
            payload = struct.pack("<BII", OTA_CMD_DATA, self.session, offset) + chunk
            self._transact(sensor_type, sensor_number, payload, 3.0)
            offset += len(chunk)
            if progress is not None:
                progress(offset, len(image))
        self._transact(sensor_type, sensor_number,
                       struct.pack("<BI", OTA_CMD_FINISH, self.session), 5.0)

    def upgrade_ab(self, sensor_type: int, sensor_number: int,
                   slot_a_path: str | os.PathLike[str],
                   slot_b_path: str | os.PathLike[str], version: int = 1,
                   stop_after: int = 0,
                   progress: ProgressCallback | None = None) -> None:
        status = self.get_status(sensor_type, sensor_number)
        if status.active_slot not in (SENSOR_OTA_SLOT_A, SENSOR_OTA_SLOT_B):
            raise OSError(errno.ENODATA, "device did not report an active slot")
        target = SENSOR_OTA_SLOT_B if status.active_slot == SENSOR_OTA_SLOT_A else SENSOR_OTA_SLOT_A
        image_path = slot_a_path if target == SENSOR_OTA_SLOT_A else slot_b_path
        self.upgrade_file(sensor_type, sensor_number, target, image_path,
                          version, stop_after, progress)

    @staticmethod
    def _raw_crc(header: bytes, payload: bytes = b"") -> int:
        return crc32(header[4:16] + payload)

    def _raw_transact(self, command: int, slot: int, offset: int = 0,
                      argument: int = 0, payload: bytes = b"",
                      timeout: float = 2.0) -> int:
        if len(payload) > OTA_CHUNK_SIZE:
            raise OSError(errno.EINVAL, "recovery payload exceeds 128 bytes")
        header = bytearray(struct.pack("<IBBHIII", RAW_MAGIC, command, slot,
                                       len(payload), offset, argument, 0))
        struct.pack_into("<I", header, 16, self._raw_crc(header, payload))
        frame = bytes(header) + payload
        self._debug_hex("RECOVERY TX", frame)
        self.port.write_all(frame, drain=True)

        window = bytearray(4)
        for _ in range(512):
            window[:] = window[1:] + self.port.read_exact(1, timeout)
            if struct.unpack("<I", window)[0] == RAW_MAGIC:
                break
        else:
            raise OSError(errno.EBADMSG, "recovery response magic not found")
        response = struct.pack("<I", RAW_MAGIC) + self.port.read_exact(RAW_HEADER_SIZE - 4, timeout)
        self._debug_hex("RECOVERY RX", response)
        if response[4] != (command | RAW_ACK) or struct.unpack_from("<I", response, 16)[0] != self._raw_crc(response):
            raise OSError(errno.EBADMSG, "invalid recovery acknowledgement")
        next_offset, status = struct.unpack_from("<II", response, 8)
        if self.debug:
            print(f"[OTA RECOVERY ACK] cmd=0x{response[4]:02X} status={status} "
                  f"({sensor_ota_device_status_string(status)}) next={next_offset}",
                  file=sys.stderr)
        if status:
            raise OtaDeviceError(status)
        return next_offset

    def recovery_upgrade_file(self, target_slot: int,
                              image_path: str | os.PathLike[str],
                              progress: ProgressCallback | None = None) -> None:
        if target_slot not in (SENSOR_OTA_SLOT_A, SENSOR_OTA_SLOT_B):
            raise OSError(errno.EINVAL, "target slot must be A or B")
        image = Path(image_path).read_bytes()
        validate_image(image, target_slot)
        self._raw_transact(RAW_CMD_PING, SENSOR_OTA_SLOT_AUTO, timeout=2.0)
        self._raw_transact(RAW_CMD_START, target_slot, argument=len(image),
                           offset=crc32(image), timeout=10.0)
        offset = 0
        while offset < len(image):
            chunk = image[offset:offset + OTA_CHUNK_SIZE]
            self._raw_transact(RAW_CMD_DATA, target_slot, offset=offset,
                               payload=chunk, timeout=3.0)
            offset += len(chunk)
            if progress is not None:
                progress(offset, len(image))
        self._raw_transact(RAW_CMD_FINISH, target_slot, timeout=5.0)


def sensor_ota_selftest() -> bool:
    test = b"123456789"
    if crc16_modbus(test) != 0x4B37 or crc32(test) != 0xCBF43926:
        return False
    frame = build_frame(OTA_FRAME_REQUEST, 0x0A, 1, bytes((OTA_CMD_STATUS,)))
    return (len(frame) == 10 and frame[0] == 0x55 and frame[9] == 0xAA
            and crc16_modbus(frame[1:7]) == struct.unpack_from("<H", frame, 7)[0])


def sensor_ota_device_status_string(status: int) -> str:
    names = ("OK", "bad frame", "bad state", "bad slot", "size error",
             "offset error", "flash error", "CRC error", "vector error",
             "session error")
    return names[status] if 0 <= status < len(names) else "unknown"


def sensor_ota_open(device: str, baud_rate: int = 115200,
                    debug: bool = False) -> SensorOta:
    return SensorOta(device, baud_rate, debug)


def sensor_ota_close(ota: SensorOta) -> None:
    ota.close()
