"""I2C access to buses declared in the board pin map."""



from ._periphery.i2c import I2C as _PeripheryI2C
from ._periphery.i2c import I2CError as _PeripheryI2CError

from dara.core._error_helpers import wrap_error_as

from .pinmap import PinMap


class I2CError(OSError):
    """Raised when an I2C operation cannot be completed."""


class I2C:
    """An I2C bus selected by its board configuration identifier.

    ``I2C(0)`` selects the ``I2C0`` entry in :class:`PinMap`. Named entries,
    such as ``I2C("I2C0")``, are also supported.
    """


    def __init__(self, id, *, auto_open = True):
        """Create an I2C bus and optionally open its mapped device."""
        self.id = id
        self.info = PinMap.get_i2c(id)
        self._periphery_instance = None
        if auto_open:
            self.open()

    @wrap_error_as(I2CError, "I2C open failed", catch=_PeripheryI2CError)
    def open(self):
        """Open or re-open the mapped I2C device."""
        self.close()
        if self.info.init_cmd is not None:
            try:
                import subprocess

                return_code = subprocess.call(self.info.init_cmd, shell=True)
            except OSError as error:
                raise I2CError("I2C init command failed") from error
            if return_code:
                raise I2CError(
                    f"I2C init command failed with exit status {return_code}"
                )
        if self.info.scl is not None and self.info.scl_func is not None:
            PinMap.set_pin_function(self.info.scl, self.info.scl_func)
        if self.info.sda is not None and self.info.sda_func is not None:
            PinMap.set_pin_function(self.info.sda, self.info.sda_func)
        self._periphery_instance = _PeripheryI2C(self.info.dev)

    @wrap_error_as(I2CError, "I2C close failed", catch=_PeripheryI2CError)
    def close(self):
        """Close the mapped I2C device."""
        if self._periphery_instance is not None:
            self._periphery_instance.close()
            self._periphery_instance = None

    def __enter__(self):
        """Open the I2C bus if needed and return it for a ``with`` statement."""
        if not self.is_opened:
            self.open()
        return self

    def __exit__(self, *args):
        """Close the I2C bus when leaving a ``with`` statement."""
        self.close()

    @property
    def is_opened(self):
        """Return whether the mapped I2C device is open."""
        return self._periphery_instance is not None

    def scan(self):
        """Return the 7-bit addresses of devices that acknowledge the bus."""
        if not self.is_opened:
            raise I2CError("I2C bus is not open")
        addresses = []
        for address in range(0x08, 0x78):
            try:
                # cvitek i2c driver does not support 0 length write probing
                # self._transfer(address, _PeripheryI2C.Message(b""))
                self._transfer(address, _PeripheryI2C.Message(b"\0", read=True))
            except I2CError:
                continue
            addresses.append(address)
        return addresses

    def read(self, addr, nbytes):
        """Read ``nbytes`` from ``addr`` and return them as bytes."""
        # I2C uses 7-bit device addresses.
        if not isinstance(addr, int) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        if nbytes < 0:
            raise ValueError("nbytes must be non-negative")

        message = _PeripheryI2C.Message(bytearray(nbytes), read=True)
        self._transfer(addr, message)
        return bytes(message.data)

    def write(self, addr, data):
        """Write ``data`` to ``addr`` and return the number of bytes written."""
        # I2C uses 7-bit device addresses.
        if not isinstance(addr, int) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")

        self._transfer(addr, _PeripheryI2C.Message(data))
        return len(data)

    def read_mem(self, addr, memaddr, nbytes):
        """Read bytes from an 8-bit register address using a combined transfer."""
        if not isinstance(addr, int) or not 0 <= addr <= 0x7F:
            raise ValueError("addr must be a 7-bit integer")
        if not isinstance(memaddr, int) or not 0 <= memaddr <= 0xFF:
            raise ValueError("memaddr must be an 8-bit integer")
        if nbytes < 0:
            raise ValueError("nbytes must be non-negative")
        register = _PeripheryI2C.Message(bytes((memaddr,)))
        data = _PeripheryI2C.Message(bytearray(nbytes), read=True)
        self._transfer(addr, register, data)
        return bytes(data.data)

    def write_mem(self, addr, memaddr, data):
        """Write bytes to an 8-bit register address and return their count."""
        if not isinstance(memaddr, int) or not 0 <= memaddr <= 0xFF:
            raise ValueError("memaddr must be an 8-bit integer")
        self.write(addr, bytes((memaddr,)) + data)
        return len(data)

    def read_byte(self, addr):
        """Read and return one byte from ``addr``."""
        return self.read(addr, 1)[0]

    def write_byte(self, addr, value):
        """Write one byte to ``addr`` and return the number of bytes written."""
        return self.write(addr, bytes((value,)))

    def read_mem_byte(self, addr, memaddr):
        """Read and return one byte from an 8-bit register address."""
        return self.read_mem(addr, memaddr, 1)[0]

    def write_mem_byte(self, addr, memaddr, value):
        """Write one byte to an 8-bit register and return the number written."""
        return self.write_mem(addr, memaddr, bytes((value,)))

    @wrap_error_as(I2CError, "I2C _transfer failed", catch=_PeripheryI2CError)
    def _transfer(self, address, *messages):
        """Execute I2C messages."""
        if self._periphery_instance is None:
            raise I2CError("I2C bus is not open")
        self._periphery_instance.transfer(address, list(messages))
