"""Tests for the driver profiling command."""

from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from profile_drivers import profile_script


class DriverProfilerTests(unittest.TestCase):
    """Verify driver profiling and report filtering."""

    def test_profile_script_reports_called_driver_function(self) -> None:
        """Report functions executed from a device directory."""
        with TemporaryDirectory() as temporary_directory:
            driver_directory = Path(temporary_directory) / "device"
            driver_directory.mkdir()
            target = driver_directory / "sample_driver.py"
            target.write_text(
                "def read_all():\n"
                "    return sum(range(10))\n"
                "\n"
                "read_all()\n",
                encoding="utf-8",
            )

            output = StringIO()
            with redirect_stdout(output):
                profile_script(target)

        report = output.getvalue()
        self.assertIn("Dara device/peripheral driver timings", report)
        self.assertIn("sample_driver.py", report)
        self.assertIn("read_all", report)


if __name__ == "__main__":
    unittest.main()
