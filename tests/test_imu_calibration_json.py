"""Regression tests for JSON-backed IMU gyro calibration profiles."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from dara.device.imu import IMU, IMUData, IMUGyroCalibration


class FakeIMU(IMU):
    """Minimal IMU implementation used without board hardware."""

    def __init__(self):
        super().__init__()
        self._opened = False

    def open(self):
        self._opened = True

    def close(self):
        self._opened = False

    @property
    def is_opened(self):
        return self._opened

    def read_imu(self, calib_gyro=True, radian=False):
        return IMUData((0.0, 0.0, 1.0), (1.0, 2.0, 3.0))


class IMUCalibrationJsonTests(unittest.TestCase):
    def test_profiles_round_trip_as_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "imu_calibration.json"
            with patch("dara.device.imu._CALIBRATION_PATH", str(path)):
                imu = FakeIMU()
                imu.save_calib_gyro(IMUGyroCalibration(1.25, -2.5, 3.75), "desk")

                self.assertTrue(path.is_file())
                self.assertEqual(
                    path.read_text(encoding="utf-8"),
                    '{"desk": {"gyro": {"x": 1.25, "y": -2.5, "z": 3.75}}}\n',
                )
                self.assertTrue(imu.calib_gyro_exists("desk"))
                calibration = imu.load_calib_gyro("desk")
                self.assertEqual((calibration.x, calibration.y, calibration.z), (1.25, -2.5, 3.75))


if __name__ == "__main__":
    unittest.main()
