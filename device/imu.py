"""Inertial measurement data types and gyro calibration support."""


from enum import Enum, IntEnum
import json
from math import atan2, degrees, isfinite, radians, sqrt
import os
from time import monotonic, sleep

from dara.core._error_helpers import wrap_error_as


class IMUMode(str, Enum):
    """Sensor outputs enabled during sampling."""

    ACC_ONLY = "acc_only"
    GYRO_ONLY = "gyro_only"
    DUAL = "dual"


class IMUAccScale(IntEnum):
    """Accelerometer full-scale ranges."""

    ACC_SCALE_2G = 2
    ACC_SCALE_4G = 4
    ACC_SCALE_8G = 8
    ACC_SCALE_16G = 16


class IMUAccOdr(Enum):
    """Accelerometer output data rates."""

    ACC_ODR_8000 = 8000
    ACC_ODR_4000 = 4000
    ACC_ODR_2000 = 2000
    ACC_ODR_1000 = 1000
    ACC_ODR_833 = 833
    ACC_ODR_500 = 500
    ACC_ODR_416 = 416
    ACC_ODR_250 = 250
    ACC_ODR_208 = 208
    ACC_ODR_128 = 128
    ACC_ODR_125 = 125
    ACC_ODR_104 = 104
    ACC_ODR_62_5 = 62.5
    ACC_ODR_52 = 52
    ACC_ODR_31_25 = 31.25
    ACC_ODR_26 = 26
    ACC_ODR_21 = 21
    ACC_ODR_12_5 = 12.5
    ACC_ODR_11 = 11
    ACC_ODR_3 = 3


class IMUGyroScale(IntEnum):
    """Gyroscope full-scale ranges."""

    GYRO_SCALE_16DPS = 16
    GYRO_SCALE_32DPS = 32
    GYRO_SCALE_64DPS = 64
    GYRO_SCALE_125DPS = 125
    GYRO_SCALE_128DPS = 128
    GYRO_SCALE_250DPS = 250
    GYRO_SCALE_256DPS = 256
    GYRO_SCALE_500DPS = 500
    GYRO_SCALE_512DPS = 512
    GYRO_SCALE_1000DPS = 1000
    GYRO_SCALE_1024DPS = 1024
    GYRO_SCALE_2000DPS = 2000
    GYRO_SCALE_2048DPS = 2048


class IMUGyroOdr(Enum):
    """Gyroscope output data rates."""

    GYRO_ODR_8000 = 8000
    GYRO_ODR_4000 = 4000
    GYRO_ODR_2000 = 2000
    GYRO_ODR_1000 = 1000
    GYRO_ODR_833 = 833
    GYRO_ODR_500 = 500
    GYRO_ODR_416 = 416
    GYRO_ODR_250 = 250
    GYRO_ODR_125 = 125
    GYRO_ODR_208 = 208
    GYRO_ODR_104 = 104
    GYRO_ODR_62_5 = 62.5
    GYRO_ODR_52 = 52
    GYRO_ODR_26 = 26
    GYRO_ODR_31_25 = 31.25
    GYRO_ODR_12_5 = 12.5


class IMUError(OSError):
    """Raised when an IMU operation cannot be completed."""


class IMUGyroCalibration:
    """Gyroscope zero-rate bias in degrees per second."""

    def __init__(self, x, y, z):
        """Initialize gyroscope bias values for all three axes."""
        self.x = x
        self.y = y
        self.z = z


class IMUData:
    """Accelerometer and gyroscope data from an IMU capability read."""

    def __init__(
        self,
        acc,
        gyro,
    ):
        """Initialize accelerometer and gyroscope samples."""
        self.acc = acc
        self.gyro = gyro


class IMUAttitude:
    """Roll, pitch, and relative yaw estimated from a six-axis IMU.

    The three public angles are degrees.  A six-axis IMU has no absolute
    heading reference, so ``yaw`` is relative to :meth:`IMUComplementaryFilter.reset`
    and will drift over time.  Use a calibrated magnetometer when an absolute
    compass heading is required.
    """

    def __init__(self, roll=0.0, pitch=0.0, yaw=0.0):
        """Create an attitude in degrees."""
        self.roll = float(roll)
        self.pitch = float(pitch)
        self.yaw = float(yaw)

    @property
    def roll_radians(self):
        """Roll in radians."""
        return radians(self.roll)

    @property
    def pitch_radians(self):
        """Pitch in radians."""
        return radians(self.pitch)

    @property
    def yaw_radians(self):
        """Relative yaw in radians."""
        return radians(self.yaw)


class IMUComplementaryFilter:
    """A conventional six-axis complementary attitude filter.

    Gyroscope input is expected in degrees per second, matching
    :meth:`read_imu` with its default ``radian=False``.  Acceleration may be
    expressed in g or m/s² because only its direction is used.
    """

    def __init__(self, correction=0.02, reference_hz=100.0):
        """Create a filter.

        ``correction`` is the accelerometer correction applied at
        ``reference_hz`` while the device is not accelerating.  The default
        0.02 is the classic 98%% gyro / 2%% accelerometer blend.  Increase it
        for less long-term tilt drift; decrease it for less response to
        linear acceleration.
        """
        if (
            isinstance(correction, bool)
            or not isinstance(correction, (int, float))
            or not 0.0 < correction <= 1.0
        ):
            raise ValueError("correction must be a number in (0.0, 1.0]")
        if (
            isinstance(reference_hz, bool)
            or not isinstance(reference_hz, (int, float))
            or reference_hz <= 0
        ):
            raise ValueError("reference_hz must be a positive number")
        self.correction = float(correction)
        self.reference_hz = float(reference_hz)
        self.reset()

    def reset(self, roll=0.0, pitch=0.0, yaw=0.0):
        """Reset attitude and timing; all angles are degrees."""
        for name, value in (("roll", roll), ("pitch", pitch), ("yaw", yaw)):
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not isfinite(value)
            ):
                raise ValueError(f"{name} must be a finite number")
        self._attitude = IMUAttitude(roll, pitch, yaw)
        self._last_time = None
        return self.attitude

    @property
    def attitude(self):
        """Return a snapshot of the current attitude in degrees."""
        return IMUAttitude(
            self._attitude.roll, self._attitude.pitch, self._attitude.yaw
        )

    def update(self, sample, dt=None):
        """Update from one :class:`IMUData` sample and return an attitude.

        Pass ``dt`` in seconds for deterministic replay/testing.  When it is
        omitted, elapsed monotonic time is used.  The first valid sample uses
        gravity to initialize roll/pitch and does not integrate yaw.
        """
        try:
            acc = tuple(sample.acc)
            gyro = tuple(sample.gyro)
        except (AttributeError, TypeError) as error:
            raise ValueError("sample must provide three-axis acc and gyro values") from error
        if len(acc) != 3 or len(gyro) != 3 or any(
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not isfinite(value)
            for value in (*acc, *gyro)
        ):
            raise ValueError("sample acc and gyro must each contain three finite numbers")

        now = monotonic()
        if dt is None:
            dt = None if self._last_time is None else now - self._last_time
        elif (
            isinstance(dt, bool)
            or not isinstance(dt, (int, float))
            or not 0.0 < dt <= 1.0
        ):
            raise ValueError("dt must be a number greater than 0 and at most 1 second")
        self._last_time = now

        roll_acc, pitch_acc = self._acc_tilt(acc)
        if dt is None:
            if roll_acc is not None:
                self._attitude.roll = roll_acc
                self._attitude.pitch = pitch_acc
            return self.attitude

        roll = self._attitude.roll + float(gyro[0]) * dt
        pitch = self._attitude.pitch + float(gyro[1]) * dt
        yaw = self._wrap_degrees(self._attitude.yaw + float(gyro[2]) * dt)
        if roll_acc is not None:
            gyro_weight = (1.0 - self.correction) ** (dt * self.reference_hz)
            roll = self._blend_angle(roll, roll_acc, gyro_weight)
            pitch = self._blend_angle(pitch, pitch_acc, gyro_weight)
        self._attitude.roll = self._wrap_degrees(roll)
        self._attitude.pitch = self._wrap_degrees(pitch)
        self._attitude.yaw = yaw
        return self.attitude

    @staticmethod
    def _acc_tilt(acc):
        """Return gravity-derived roll/pitch, or ``(None, None)`` when invalid."""
        ax, ay, az = (float(value) for value in acc)
        norm = sqrt(ax * ax + ay * ay + az * az)
        if norm <= 1e-9:
            return None, None
        return degrees(atan2(ay, az)), degrees(atan2(-ax, sqrt(ay * ay + az * az)))

    @staticmethod
    def _wrap_degrees(value):
        """Normalize an angle to [-180, 180)."""
        return (value + 180.0) % 360.0 - 180.0

    @classmethod
    def _blend_angle(cls, predicted, measured, gyro_weight):
        """Blend the shortest angular difference without 180-degree jumps."""
        return predicted + (1.0 - gyro_weight) * cls._wrap_degrees(measured - predicted)


_CALIBRATION_PATH = "/data/etc/dara/imu_calibration.json"


class GyroCalibration:
    """Reusable gyro calibration behavior for concrete IMU drivers.

    This is not a hardware-device abstraction.  QMI8658 and LSM6DSOWTR use
    the same persisted calibration format while each owns its I2C lifecycle.
    """

    def __init__(self):
        """Initialize shared inertial measurement unit state."""
        self.mode = None
        self.calib_gyro_data = IMUGyroCalibration(0.0, 0.0, 0.0)

    def _calibrate_gyro(
        self,
        gyro,
        calib_gyro = True,
        radian = False,
    ):
        """Validate read options, apply gyro bias, and optionally convert units."""
        if not isinstance(calib_gyro, bool):
            raise ValueError("calib_gyro must be a boolean")
        if not isinstance(radian, bool):
            raise ValueError("radian must be a boolean")
        if self.mode is IMUMode.ACC_ONLY:  # pyright: ignore[reportAttributeAccessIssue]
            return gyro
        if calib_gyro:
            gyro = (
                gyro[0] - self.calib_gyro_data.x,
                gyro[1] - self.calib_gyro_data.y,
                gyro[2] - self.calib_gyro_data.z,
            )
        if radian:
            gyro = (radians(gyro[0]), radians(gyro[1]), radians(gyro[2]))
        return gyro

    @wrap_error_as(IMUError, "IMU calibration failed", catch=OSError)
    def calib_gyro(
        self,
        time_ms,
        interval_ms = -1,
        save_id = "default",
    ):
        """Calculate gyro bias, optionally save it, and make it active."""
        if isinstance(time_ms, bool) or not isinstance(time_ms, int) or time_ms <= 0:
            raise ValueError("time_ms must be a positive integer")
        if (
            isinstance(interval_ms, bool)
            or not isinstance(interval_ms, int)
            or interval_ms < -1
        ):
            raise ValueError(
                "interval_ms must be an integer greater than or equal to -1"
            )
        if not isinstance(save_id, str):
            raise ValueError("save_id must be a string")
        if self.mode is IMUMode.ACC_ONLY:  # pyright: ignore[reportAttributeAccessIssue]
            raise IMUError("gyro calibration requires gyro output")

        totals = [0.0, 0.0, 0.0]
        count = 0
        deadline = monotonic() + time_ms / 1_000
        while True:
            sample = self.read_imu(calib_gyro=False, radian=False)
            for index, value in enumerate(sample.gyro):  # pyright: ignore[reportAttributeAccessIssue]
                totals[index] += value
            count += 1
            if monotonic() >= deadline:
                break
            if interval_ms > 0:
                sleep(interval_ms / 1_000)

        calibration = IMUGyroCalibration(*(total / count for total in totals))
        self.calib_gyro_data = calibration
        if save_id:
            self.save_calib_gyro(calibration, save_id)
        return calibration

    @wrap_error_as(
        IMUError,
        "IMU calibration data could not be read",
        catch=(OSError, json.JSONDecodeError),
    )
    def calib_gyro_exists(self, save_id = "default"):
        """Return whether a valid named gyro calibration is saved."""
        self._validate_save_id(save_id)
        return self._profile_calibration(self._load_profiles(), save_id) is not None

    @wrap_error_as(
        IMUError,
        "IMU calibration data could not be read",
        catch=(OSError, json.JSONDecodeError),
    )
    def load_calib_gyro(self, save_id = "default"):
        """Load and activate a named gyro calibration, or zero bias if absent."""
        self._validate_save_id(save_id)
        calibration = self._profile_calibration(self._load_profiles(), save_id)
        if calibration is None:
            calibration = IMUGyroCalibration(0.0, 0.0, 0.0)
        self.calib_gyro_data = calibration
        return calibration

    @wrap_error_as(
        IMUError,
        "IMU calibration data could not be saved",
        catch=(OSError, TypeError, ValueError),
    )
    def save_calib_gyro(
        self,
        calibration,
        save_id = "default",
    ):
        """Save a gyro calibration under ``save_id`` and preserve other profiles."""
        self._validate_save_id(save_id)
        if not isinstance(calibration, IMUGyroCalibration):
            raise ValueError("calibration must be a GyroCalibration value")

        profiles = self._load_profiles()
        profile = profiles.setdefault(save_id, {})
        if not isinstance(profile, dict):
            raise IMUError(f"calibration profile '{save_id}' must be a mapping")
        profile_data = profile
        profile_data["gyro"] = {
            "x": calibration.x,
            "y": calibration.y,
            "z": calibration.z,
        }
        path = _CALIBRATION_PATH
        os.makedirs(os.path.dirname(path), exist_ok=True)
        temporary_path = f"{path}.tmp"
        with open(temporary_path, "w", encoding="utf-8") as file:
            json.dump(profiles, file, sort_keys=True, allow_nan=False)
            file.write("\n")
        os.replace(temporary_path, path)

    @staticmethod
    def _validate_save_id(save_id):
        """Reject invalid calibration profile identifiers."""
        if not isinstance(save_id, str) or not save_id:
            raise ValueError("save_id must be a non-empty string")

    @staticmethod
    def _load_profiles():
        """Load the JSON calibration profile mapping."""
        try:
            with open(_CALIBRATION_PATH, encoding="utf-8") as file:
                profiles = json.load(file)
        except FileNotFoundError:
            return {}
        if profiles is None:
            return {}
        if not isinstance(profiles, dict) or not all(
            isinstance(save_id, str) for save_id in profiles
        ):
            raise IMUError("IMU calibration data must be a string-keyed mapping")
        return profiles

    @staticmethod
    def _profile_calibration(
        profiles, save_id
    ):
        """Return one validated gyro calibration from a profile mapping."""
        profile = profiles.get(save_id)
        if profile is None:
            return None
        if not isinstance(profile, dict):
            raise IMUError(f"calibration profile '{save_id}' has no gyro mapping")
        profile_data = profile
        gyro_value = profile_data.get("gyro")
        if not isinstance(gyro_value, dict):
            raise IMUError(f"calibration profile '{save_id}' has no gyro mapping")
        gyro = gyro_value
        values = []
        for axis in ("x", "y", "z"):
            value = gyro.get(axis)
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not isfinite(value)
            ):
                raise IMUError(f"calibration profile '{save_id}' has invalid gyro bias")
            values.append(float(value))
        return IMUGyroCalibration(values[0], values[1], values[2])
