"""Profile Dara device and peripheral driver calls made by a Python script."""

from __future__ import annotations

import argparse
import cProfile
from pathlib import Path
import pstats
import runpy
import sys
from collections.abc import Sequence


_DRIVER_PATH_PATTERN = r"[\\/](?:device|peripheral)[\\/]"


def _run_target(target: Path, arguments: Sequence[str]) -> None:
    """Run a target script with normal script path and argument semantics."""
    original_argv = sys.argv
    original_path = sys.path.copy()
    sys.argv = [str(target), *arguments]
    sys.path.insert(0, str(target.parent))
    try:
        runpy.run_path(str(target), run_name="__main__")
    finally:
        sys.argv = original_argv
        sys.path[:] = original_path


def profile_script(target: Path, arguments: Sequence[str] = ()) -> None:
    """Run ``target`` once and print timing for called Dara driver functions."""
    target = target.resolve()
    if not target.is_file():
        raise FileNotFoundError(f"target script does not exist: {target}")

    profiler = cProfile.Profile()
    try:
        profiler.runcall(_run_target, target, arguments)
    finally:
        print("\nDara device/peripheral driver timings")
        print("ncalls: calls, tottime: function time, cumtime: cumulative time")
        pstats.Stats(profiler).sort_stats(pstats.SortKey.CUMULATIVE).print_stats(
            _DRIVER_PATH_PATTERN
        )


def main(argv: Sequence[str] | None = None) -> int:
    """Parse command-line arguments and profile the requested script."""
    parser = argparse.ArgumentParser(
        description=(
            "Run a Python script once and report calls in Dara's device and "
            "peripheral driver directories."
        )
    )
    parser.add_argument("script", type=Path, help="Python script to run")
    parser.add_argument(
        "script_args",
        nargs=argparse.REMAINDER,
        help="arguments passed to the target script",
    )
    args = parser.parse_args(argv)
    profile_script(args.script, args.script_args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
