"""Board pin and peripheral configuration loading."""


import os
import sys

from dara.peripheral.pinmux import PinMux


class PinInfo:
    """Pin-multiplexing register details loaded from a board configuration."""


    def __init__(
        self,
        name,
        reg_addr,
        bit_offset,
        bit_width,
        func_regs,
        funcs,
    ):
        """Initialize pin-multiplexing register details."""
        self.name = name
        self.reg_addr = reg_addr
        self.bit_offset = bit_offset
        self.bit_width = bit_width
        self.func_regs = func_regs
        self.funcs = funcs


class UARTInfo:
    """Device and pin assignments for a configured UART."""

    init_cmd = None
    """The shell command run before opening the UART, or ``None``."""

    def __init__(
        self,
        dev,
        tx,
        rx,
        tx_func,
        rx_func,
        init_cmd = None,
    ):
        """Initialize a UART definition."""
        self.dev = dev
        self.tx = tx
        self.rx = rx
        self.tx_func = tx_func
        self.rx_func = rx_func
        self.init_cmd = init_cmd


class I2CInfo:
    """Device and pin assignments for a configured I2C bus."""

    init_cmd = None
    """The shell command run before opening the I2C bus, or ``None``."""

    def __init__(
        self,
        dev,
        scl,
        sda,
        scl_func,
        sda_func,
        init_cmd = None,
    ):
        """Initialize an I2C definition."""
        self.dev = dev
        self.scl = scl
        self.sda = sda
        self.scl_func = scl_func
        self.sda_func = sda_func
        self.init_cmd = init_cmd


class SPIInfo:
    """Device and pin assignments for a configured SPI bus."""

    init_cmd = None
    """The shell command run before opening the SPI bus, or ``None``."""

    def __init__(
        self,
        dev,
        sclk,
        mosi,
        miso,
        cs,
        sclk_func,
        mosi_func,
        miso_func,
        cs_func,
        init_cmd = None,
    ):
        """Initialize an SPI definition."""
        self.dev = dev
        self.sclk = sclk
        self.mosi = mosi
        self.miso = miso
        self.cs = cs
        self.sclk_func = sclk_func
        self.mosi_func = mosi_func
        self.miso_func = miso_func
        self.cs_func = cs_func
        self.init_cmd = init_cmd


class GPIOInfo:
    """Chip and line assignments for a configured GPIO."""

    init_cmd = None
    """The shell command run before opening the GPIO line, or ``None``."""

    def __init__(
        self,
        pin,
        pin_func,
        chip,
        num,
        init_cmd = None,
    ):
        """Initialize a GPIO definition."""
        self.pin = pin
        self.pin_func = pin_func
        self.chip = chip
        self.num = num
        self.init_cmd = init_cmd


class SoftKeyInfo:
    """GPIO or ADC assignment for a configured soft key."""

    """The configured GPIO identifier, or ``None`` for an ADC-backed key."""
    init_cmd = None
    """The shell command run before opening the soft key, or ``None``."""
    adc = None
    """The configured ADC identifier, or ``None`` for a GPIO-backed key."""
    raw_values = ()
    """ADC raw values that represent the key's down state."""
    raw_tolerance = 100
    """The inclusive tolerance applied around each configured ADC raw value."""

    def __init__(
        self,
        gpio,
        init_cmd = None,
        adc = None,
        raw_values = (),
        raw_tolerance = 100,
    ):
        """Initialize a soft-key definition."""
        self.gpio = gpio
        self.init_cmd = init_cmd
        self.adc = adc
        self.raw_values = raw_values
        self.raw_tolerance = raw_tolerance


class PWMInfo:
    """Chip and channel assignments for a configured PWM output."""

    init_cmd = None
    """The shell command run before opening the PWM output, or ``None``."""

    def __init__(
        self,
        pin,
        pin_func,
        chip,
        num,
        freq,
        duty_cycle,
        enable,
        init_cmd = None,
    ):
        """Initialize a PWM definition."""
        self.pin = pin
        self.pin_func = pin_func
        self.chip = chip
        self.num = num
        self.freq = freq
        self.duty_cycle = duty_cycle
        self.enable = enable
        self.init_cmd = init_cmd


class ADCInfo:
    """Pin assignment, sysfs path, and defaults for a configured ADC input."""

    init_cmd = None
    """The shell command run before opening the ADC input, or ``None``."""

    def __init__(
        self,
        pin,
        pin_func,
        sysfs,
        resolution,
        vref,
        init_cmd = None,
    ):
        """Initialize an ADC definition."""
        self.pin = pin
        self.pin_func = pin_func
        self.sysfs = sysfs
        self.resolution = resolution
        self.vref = vref
        self.init_cmd = init_cmd


class WDTInfo:
    """Configuration for the watchdog timer."""

    init_cmd = None
    """The shell command run before opening the watchdog, or ``None``."""

    def __init__(self, init_cmd = None):
        """Initialize a watchdog definition."""
        self.init_cmd = init_cmd


class BoardInfo:
    """Descriptive metadata for a board configuration."""

    def __init__(
        self,
        id,
        name,
        description,
        platform,
        board_version,
    ):
        """Initialize descriptive board metadata."""
        self.id = id
        self.name = name
        self.description = description
        self.platform = platform
        self.board_version = board_version


class _Default:
    """Private marker for omitted pin-map constructor arguments."""


_DEFAULT = _Default()


class PinMapConfig:
    """Strongly typed pin and peripheral definitions for a board."""

    def __init__(
        self,
        pins = _DEFAULT,
        adc = _DEFAULT,
        softkey = _DEFAULT,
        gpio = _DEFAULT,
        i2c = _DEFAULT,
        pwm = _DEFAULT,
        spi = _DEFAULT,
        uart = _DEFAULT,
        wdt = _DEFAULT,
    ):
        """Initialize pin-map sections with independent omitted defaults."""
        self.pins = {} if isinstance(pins, _Default) else pins
        self.adc = {} if isinstance(adc, _Default) else adc
        self.softkey = {} if isinstance(softkey, _Default) else softkey
        self.gpio = {} if isinstance(gpio, _Default) else gpio
        self.i2c = {} if isinstance(i2c, _Default) else i2c
        self.pwm = {} if isinstance(pwm, _Default) else pwm
        self.spi = {} if isinstance(spi, _Default) else spi
        self.uart = {} if isinstance(uart, _Default) else uart
        self.wdt = WDTInfo() if isinstance(wdt, _Default) else wdt


class BoardConfig:
    """Versioned board metadata and pin-map configuration."""

    def __init__(
        self, config_version, board, pinmap
    ):
        """Initialize a versioned board configuration."""
        self.config_version = config_version
        self.board = board
        self.pinmap = pinmap


class PinMap:
    """The pin and peripheral definitions for the active board configuration."""

    pins = {}
    adc = {}
    softkey = {}
    gpio = {}
    i2c = {}
    pwm = {}
    spi = {}
    uart = {}
    wdt = WDTInfo()
    _initialized = False

    @classmethod
    def init(cls, board_cfg = None):
        """Load a board configuration and replace the active pin map.

        The selected ``.py`` file is trusted executable code and must export a
        :class:`BoardConfig` as ``BOARD_CONFIG``. An explicitly supplied path
        takes precedence. Otherwise, look beside the entry-point script, then
        use ``/etc/board.py``. Active maps change only after full validation.
        Section dictionaries are copied shallowly, so entry and watchdog
        mutations remain visible and are not automatically revalidated.
        """
        board_cfg_path = cls._board_config_path(board_cfg)
        config = cls._load_board_config(board_cfg_path)
        cls._validate_board_config(config, board_cfg_path)
        pinmap = config.pinmap

        # Detach section dictionaries while preserving their entry objects.
        pins = dict(pinmap.pins)
        adc = dict(pinmap.adc)
        softkey = dict(pinmap.softkey)
        gpio = dict(pinmap.gpio)
        i2c = dict(pinmap.i2c)
        pwm = dict(pinmap.pwm)
        spi = dict(pinmap.spi)
        uart = dict(pinmap.uart)
        wdt = pinmap.wdt

        # Replace all maps only after the full file has been validated.
        cls.pins = pins
        cls.adc = adc
        cls.softkey = softkey
        cls.gpio = gpio
        cls.i2c = i2c
        cls.pwm = pwm
        cls.spi = spi
        cls.uart = uart
        cls.wdt = wdt
        cls._initialized = True

    @classmethod
    def get_uart(cls, identifier):
        """Return the definition for an integer or named UART identifier."""
        if not cls._initialized:
            cls.init()

        name = cls._uart_name(identifier)
        try:
            return cls.uart[name]
        except KeyError as error:
            available = ", ".join(sorted(cls.uart)) or "none"
            raise ValueError(
                f"unknown UART '{name}'; configured UARTs: {available}"
            ) from error

    @classmethod
    def get_adc(cls, identifier):
        """Return the definition for an integer or named ADC identifier."""
        if not cls._initialized:
            cls.init()
        name = cls._adc_name(identifier)
        try:
            return cls.adc[name]
        except KeyError as error:
            available = ", ".join(sorted(cls.adc)) or "none"
            raise ValueError(
                f"unknown ADC '{name}'; configured ADCs: {available}"
            ) from error

    @classmethod
    def get_wdt(cls):
        """Return the watchdog configuration."""
        if not cls._initialized:
            cls.init()
        return cls.wdt

    @classmethod
    def get_pins(cls):
        """Return the configured pin identifiers."""
        if not cls._initialized:
            cls.init()
        return tuple(cls.pins)

    @classmethod
    def get_pin_functions(cls, pin):
        """Return the functions supported by a configured pin."""
        return tuple(cls._get_pin(pin).funcs)

    @classmethod
    def set_pin_function(cls, pin, function):
        """Select a configured function for a pin."""
        info = cls._get_pin(pin)
        if not isinstance(function, str) or not function:
            raise ValueError("pin function must be a non-empty string")
        try:
            value = info.func_regs[info.funcs.index(function)]
        except ValueError as error:
            available = ", ".join(info.funcs) or "none"
            raise ValueError(
                f"pin '{pin}' does not support '{function}'; available functions: {available}"
            ) from error
        PinMux.set_bits(info.reg_addr, info.bit_offset, info.bit_width, value)

    @classmethod
    def get_gpio(cls, identifier):
        """Return the definition for an integer or named GPIO identifier."""
        if not cls._initialized:
            cls.init()
        name = cls._gpio_name(identifier)
        try:
            return cls.gpio[name]
        except KeyError as error:
            available = ", ".join(sorted(cls.gpio)) or "none"
            raise ValueError(
                f"unknown GPIO '{name}'; configured GPIOs: {available}"
            ) from error

    @classmethod
    def get_softkey(cls, identifier):
        """Return the definition for a named softkey identifier."""
        if not cls._initialized:
            cls.init()
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("softkey identifier must be a non-empty string")
        try:
            return cls.softkey[identifier]
        except KeyError as error:
            available = ", ".join(sorted(cls.softkey)) or "none"
            raise ValueError(
                f"unknown softkey '{identifier}'; configured softkeys: {available}"
            ) from error

    @classmethod
    def get_pwm(cls, identifier):
        """Return the definition for an integer or named PWM identifier."""
        if not cls._initialized:
            cls.init()
        name = cls._pwm_name(identifier)
        try:
            return cls.pwm[name]
        except KeyError as error:
            available = ", ".join(sorted(cls.pwm)) or "none"
            raise ValueError(
                f"unknown PWM '{name}'; configured PWMs: {available}"
            ) from error

    @classmethod
    def get_i2c(cls, identifier):
        """Return the definition for an integer or named I2C identifier."""
        if not cls._initialized:
            cls.init()

        name = cls._i2c_name(identifier)
        try:
            return cls.i2c[name]
        except KeyError as error:
            available = ", ".join(sorted(cls.i2c)) or "none"
            raise ValueError(
                f"unknown I2C '{name}'; configured I2C buses: {available}"
            ) from error

    @classmethod
    def get_spi(cls, identifier, cs = None):
        """Return the definition for an SPI bus and optional chip select."""
        if not cls._initialized:
            cls.init()

        name = cls._spi_name(identifier, cs)
        try:
            return cls.spi[name]
        except KeyError as error:
            available = ", ".join(sorted(cls.spi)) or "none"
            raise ValueError(
                f"unknown SPI '{name}'; configured SPI buses: {available}"
            ) from error

    @staticmethod
    def _board_config_path(board_cfg):
        """Return an explicit, bundled, entry-point, or system board configuration."""
        if board_cfg is not None:
            path = os.fspath(board_cfg)
        else:
            local_path = os.path.join(os.path.dirname(sys.argv[0]), "board.py")
            bundled_path = os.path.join(
                os.path.dirname(__file__), "cv184x_mainboard.py"
            )
            if os.path.isfile(local_path):
                path = local_path
            elif os.path.isfile(bundled_path):
                path = bundled_path
            else:
                path = "/etc/board.py"

        if not os.path.isfile(path):
            raise FileNotFoundError(f"board config '{path}' does not exist")
        if os.path.splitext(path)[1] != ".py":
            raise ValueError(f"board config '{path}' must be a Python (.py) file")
        return path

    @staticmethod
    def _load_board_config(board_cfg_path):
        """Execute a Python board file and return its exported configuration."""
        namespace = {
            "__file__": board_cfg_path,
            "__name__": "__dara_board_config__",
        }
        with open(board_cfg_path, "rb") as file:
            source = file.read()
        exec(compile(source, board_cfg_path, "exec"), namespace)

        if "BOARD_CONFIG" not in namespace:
            raise ValueError(
                f"board config '{board_cfg_path}' must export BOARD_CONFIG"
            )
        config = namespace["BOARD_CONFIG"]
        if not isinstance(config, BoardConfig):
            raise ValueError(
                f"BOARD_CONFIG in '{board_cfg_path}' must be a BoardConfig"
            )
        return config

    @staticmethod
    def _config_error(board_cfg_path, message):
        """Create a path-qualified board configuration error."""
        return ValueError(f"{message} in board config '{board_cfg_path}'")

    @classmethod
    def _validate_board_config(
        cls, config, board_cfg_path
    ):
        """Validate every field and reference in a board configuration."""
        if (
            isinstance(config.config_version, bool)
            or not isinstance(config.config_version, int)
            or config.config_version != 1
        ):
            raise cls._config_error(board_cfg_path, "unsupported config_version")
        if not isinstance(config.board, BoardInfo):
            raise cls._config_error(board_cfg_path, "board must be a BoardInfo")
        board_fields = (
            config.board.id,
            config.board.name,
            config.board.description,
            config.board.platform,
            config.board.board_version,
        )
        if not all(isinstance(value, str) and value for value in board_fields):
            raise cls._config_error(board_cfg_path, "board has invalid metadata")
        if not isinstance(config.pinmap, PinMapConfig):
            raise cls._config_error(board_cfg_path, "pinmap must be a PinMapConfig")

        pinmap = config.pinmap
        sections = (
            ("pins", pinmap.pins, PinInfo),
            ("adc", pinmap.adc, ADCInfo),
            ("softkey", pinmap.softkey, SoftKeyInfo),
            ("gpio", pinmap.gpio, GPIOInfo),
            ("i2c", pinmap.i2c, I2CInfo),
            ("pwm", pinmap.pwm, PWMInfo),
            ("spi", pinmap.spi, SPIInfo),
            ("uart", pinmap.uart, UARTInfo),
        )
        for name, mapping, value_type in sections:
            cls._validate_mapping(name, mapping, value_type, board_cfg_path)
        if not isinstance(pinmap.wdt, WDTInfo):
            raise cls._config_error(board_cfg_path, "wdt must be a WDTInfo")

        cls._validate_pins(pinmap.pins, board_cfg_path)
        cls._validate_adc(pinmap.adc, pinmap.pins, board_cfg_path)
        cls._validate_gpio(pinmap.gpio, pinmap.pins, board_cfg_path)
        cls._validate_softkeys(
            pinmap.softkey, pinmap.gpio, pinmap.adc, board_cfg_path
        )
        cls._validate_i2c(pinmap.i2c, pinmap.pins, board_cfg_path)
        cls._validate_pwm(pinmap.pwm, pinmap.pins, board_cfg_path)
        cls._validate_spi(pinmap.spi, pinmap.pins, board_cfg_path)
        cls._validate_uarts(pinmap.uart, pinmap.pins, board_cfg_path)
        cls._validate_init_cmd("WDT", pinmap.wdt.init_cmd, board_cfg_path)

    @classmethod
    def _validate_mapping(
        cls,
        name,
        mapping,
        value_type,
        board_cfg_path,
    ):
        """Validate one identifier-to-information mapping."""
        if not isinstance(mapping, dict):
            raise cls._config_error(board_cfg_path, f"pinmap.{name} must be a dict")
        for identifier, value in mapping.items():
            if not isinstance(identifier, str) or not identifier:
                raise cls._config_error(
                    board_cfg_path, f"pinmap.{name} has an invalid identifier"
                )
            if not isinstance(value, value_type):
                raise cls._config_error(
                    board_cfg_path,
                    f"pinmap.{name}['{identifier}'] must be a {value_type.__name__}",
                )

    @classmethod
    def _validate_init_cmd(
        cls, label, value, board_cfg_path
    ):
        """Validate an optional peripheral initialization command."""
        if value is not None and not isinstance(value, str):
            raise cls._config_error(
                board_cfg_path, f"{label} has an invalid init command"
            )

    @classmethod
    def _get_pin(cls, pin):
        """Return the configuration for a pin identifier."""
        if not cls._initialized:
            cls.init()
        if not isinstance(pin, str) or not pin:
            raise ValueError("pin must be a non-empty string")
        try:
            return cls.pins[pin]
        except KeyError as error:
            available = ", ".join(cls.pins) or "none"
            raise ValueError(f"unknown pin '{pin}'; configured pins: {available}") from error

    @staticmethod
    def _valid_pin_function(
        pin, function, pins
    ):
        """Return whether a pin/function pair is both omitted or valid."""
        return (pin is None and function is None) or (
            isinstance(pin, str)
            and pin in pins
            and isinstance(function, str)
            and function in pins[pin].funcs
        )

    @staticmethod
    def _validate_pins(value, board_cfg_path):
        """Validate configured pin definitions."""
        for name, info in value.items():
            if (
                not all(
                    isinstance(item, int) and not isinstance(item, bool)
                    for item in (info.reg_addr, info.bit_offset, info.bit_width)
                )
                or info.reg_addr < 0
                or not 0 <= info.bit_offset < 32
                or not 1 <= info.bit_width <= 32 - info.bit_offset
                or not isinstance(info.name, str)
                or not info.name
                or not isinstance(info.func_regs, tuple)
                or not isinstance(info.funcs, tuple)
                or not all(
                    isinstance(item, int)
                    and not isinstance(item, bool)
                    and 0 <= item < 1 << info.bit_width
                    for item in info.func_regs
                )
                or not all(isinstance(item, str) and item for item in info.funcs)
                or len(info.func_regs) != len(info.funcs)
            ):
                raise PinMap._config_error(
                    board_cfg_path, f"pin '{name}' has an invalid function definition"
                )

    @classmethod
    def _validate_adc(
        cls,
        value,
        pins,
        board_cfg_path,
    ):
        """Validate configured ADC definitions."""
        for identifier, info in value.items():
            cls._validate_init_cmd(f"ADC '{identifier}'", info.init_cmd, board_cfg_path)
            if (
                not cls._valid_pin_function(info.pin, info.pin_func, pins)
                or not isinstance(info.sysfs, str)
                or not info.sysfs
                or isinstance(info.resolution, bool)
                or not isinstance(info.resolution, int)
                or info.resolution <= 0
                or isinstance(info.vref, bool)
                or not isinstance(info.vref, (int, float))
                or info.vref <= 0
            ):
                raise cls._config_error(
                    board_cfg_path,
                    f"ADC '{identifier}' has an invalid pin, sysfs path, resolution, or vref",
                )

    @classmethod
    def _validate_gpio(
        cls,
        value,
        pins,
        board_cfg_path,
    ):
        """Validate configured GPIO definitions."""
        for identifier, info in value.items():
            cls._validate_init_cmd(f"GPIO '{identifier}'", info.init_cmd, board_cfg_path)
            if (
                not cls._valid_pin_function(info.pin, info.pin_func, pins)
                or isinstance(info.chip, bool)
                or not isinstance(info.chip, int)
                or isinstance(info.num, bool)
                or not isinstance(info.num, int)
                or info.chip < 0
                or info.num < 0
            ):
                raise cls._config_error(
                    board_cfg_path,
                    f"GPIO '{identifier}' has an invalid pin, chip, or number",
                )

    @classmethod
    def _validate_softkeys(
        cls,
        value,
        gpio,
        adc,
        board_cfg_path,
    ):
        """Validate configured GPIO and ADC soft-key definitions."""
        for identifier, info in value.items():
            cls._validate_init_cmd(
                f"softkey '{identifier}'", info.init_cmd, board_cfg_path
            )
            if (info.gpio is None) == (info.adc is None):
                raise cls._config_error(
                    board_cfg_path,
                    f"softkey '{identifier}' must configure exactly one of 'gpio' or 'adc'",
                )
            if info.gpio is not None:
                if info.raw_values != () or info.raw_tolerance != 100:
                    raise cls._config_error(
                        board_cfg_path,
                        f"softkey '{identifier}' has ADC fields without an ADC",
                    )
                if isinstance(info.gpio, str) and info.gpio in gpio:
                    continue
                raise cls._config_error(
                    board_cfg_path,
                    f"softkey '{identifier}' references an undefined GPIO",
                )

            if not isinstance(info.adc, str) or info.adc not in adc:
                raise cls._config_error(
                    board_cfg_path,
                    f"softkey '{identifier}' references an undefined ADC",
                )
            max_raw = (1 << adc[info.adc].resolution) - 1
            if (
                not isinstance(info.raw_values, tuple)
                or not info.raw_values
                or any(
                    isinstance(raw, bool)
                    or not isinstance(raw, int)
                    or not 0 <= raw <= max_raw
                    for raw in info.raw_values
                )
            ):
                raise cls._config_error(
                    board_cfg_path,
                    f"softkey '{identifier}' has invalid ADC raw values",
                )
            if (
                isinstance(info.raw_tolerance, bool)
                or not isinstance(info.raw_tolerance, int)
                or not 0 <= info.raw_tolerance <= max_raw
            ):
                raise cls._config_error(
                    board_cfg_path,
                    f"softkey '{identifier}' has invalid ADC raw tolerance",
                )

    @classmethod
    def _validate_pwm(
        cls,
        value,
        pins,
        board_cfg_path,
    ):
        """Validate configured PWM definitions."""
        for identifier, info in value.items():
            cls._validate_init_cmd(f"PWM '{identifier}'", info.init_cmd, board_cfg_path)
            if (
                not cls._valid_pin_function(info.pin, info.pin_func, pins)
                or isinstance(info.chip, bool)
                or not isinstance(info.chip, int)
                or isinstance(info.num, bool)
                or not isinstance(info.num, int)
                or info.chip < 0
                or info.num < 0
                or isinstance(info.freq, bool)
                or not isinstance(info.freq, (int, float))
                or info.freq <= 0
                or isinstance(info.duty_cycle, bool)
                or not isinstance(info.duty_cycle, (int, float))
                or not 0 <= info.duty_cycle <= 1
                or not isinstance(info.enable, bool)
            ):
                raise cls._config_error(
                    board_cfg_path,
                    f"PWM '{identifier}' has an invalid pin, chip, number, frequency, duty cycle, or enable state",
                )

    @classmethod
    def _validate_i2c(
        cls,
        value,
        pins,
        board_cfg_path,
    ):
        """Validate configured I2C definitions."""
        for name, info in value.items():
            cls._validate_init_cmd(f"I2C '{name}'", info.init_cmd, board_cfg_path)
            if (
                not isinstance(info.dev, str)
                or not info.dev
                or not cls._valid_pin_function(info.scl, info.scl_func, pins)
                or not cls._valid_pin_function(info.sda, info.sda_func, pins)
            ):
                raise cls._config_error(
                    board_cfg_path, f"I2C '{name}' has an invalid device or pin name"
                )

    @classmethod
    def _validate_spi(
        cls,
        value,
        pins,
        board_cfg_path,
    ):
        """Validate configured SPI definitions."""
        for name, info in value.items():
            cls._validate_init_cmd(f"SPI '{name}'", info.init_cmd, board_cfg_path)
            if (
                not isinstance(info.dev, str)
                or not info.dev
                or not cls._valid_pin_function(info.sclk, info.sclk_func, pins)
                or not cls._valid_pin_function(info.mosi, info.mosi_func, pins)
                or not cls._valid_pin_function(info.miso, info.miso_func, pins)
                or not cls._valid_pin_function(info.cs, info.cs_func, pins)
            ):
                raise cls._config_error(
                    board_cfg_path, f"SPI '{name}' has an invalid device or pin name"
                )

    @classmethod
    def _validate_uarts(
        cls,
        value,
        pins,
        board_cfg_path,
    ):
        """Validate configured UART definitions."""
        for name, info in value.items():
            cls._validate_init_cmd(f"UART '{name}'", info.init_cmd, board_cfg_path)
            if not isinstance(info.dev, str) or not info.dev:
                raise cls._config_error(
                    board_cfg_path, f"UART '{name}' has an invalid device or pin name"
                )
            if (
                not cls._valid_pin_function(info.tx, info.tx_func, pins)
                or not cls._valid_pin_function(info.rx, info.rx_func, pins)
            ):
                raise cls._config_error(
                    board_cfg_path,
                    f"UART '{name}' references a pin that is not defined",
                )

    @staticmethod
    def _uart_name(identifier):
        """Normalize an integer or numeric string identifier to a UART map key."""
        if isinstance(identifier, int):
            return f"UART{identifier}"
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("UART identifier must be an integer or non-empty string")
        return f"UART{identifier}" if identifier.isdigit() else identifier

    @staticmethod
    def _adc_name(identifier):
        """Normalize an integer or numeric string identifier to an ADC map key."""
        if isinstance(identifier, int):
            return f"ADC{identifier}"
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("ADC identifier must be an integer or non-empty string")
        return f"ADC{identifier}" if identifier.isdigit() else identifier

    @staticmethod
    def _i2c_name(identifier):
        """Normalize an integer or numeric string identifier to an I2C map key."""
        if isinstance(identifier, int):
            return f"I2C{identifier}"
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("I2C identifier must be an integer or non-empty string")
        return f"I2C{identifier}" if identifier.isdigit() else identifier

    @staticmethod
    def _spi_name(identifier, cs):
        """Normalize an SPI bus and optional hardware CS to a map key."""
        if isinstance(identifier, int):
            bus = f"SPI{identifier}"
        elif isinstance(identifier, str) and identifier:
            bus = f"SPI{identifier}" if identifier.isdigit() else identifier
        else:
            raise ValueError("SPI identifier must be an integer or non-empty string")
        if cs is None:
            return bus
        if isinstance(cs, int):
            return f"{bus}.{cs}"
        if isinstance(cs, str) and cs.isdigit():
            return f"{bus}.{cs}"
        raise ValueError("SPI chip select must be an integer or numeric string")

    @staticmethod
    def _gpio_name(identifier):
        """Normalize an integer or numeric string identifier to a GPIO map key."""
        if isinstance(identifier, int):
            return f"GPIO{identifier}"
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("GPIO identifier must be an integer or non-empty string")
        return f"GPIO{identifier}" if identifier.isdigit() else identifier

    @staticmethod
    def _pwm_name(identifier):
        """Normalize an integer or numeric string identifier to a PWM map key."""
        if isinstance(identifier, int):
            return f"PWM{identifier}"
        if not isinstance(identifier, str) or not identifier:
            raise ValueError("PWM identifier must be an integer or non-empty string")
        return f"PWM{identifier}" if identifier.isdigit() else identifier
