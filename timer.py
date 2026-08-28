"""Provide a small helper for measuring callable execution time."""

from __future__ import annotations

from collections.abc import Callable
from functools import wraps
from time import perf_counter
from typing import Any, ParamSpec, TypeVar


ParamsT = ParamSpec("ParamsT")
ResultT = TypeVar("ResultT")


def measure_time(
    function: Callable[ParamsT, ResultT],
) -> Callable[ParamsT, ResultT]:
    """Decorate a callable to print elapsed time after every invocation."""

    @wraps(function)
    def wrapper(*args: ParamsT.args, **kwargs: ParamsT.kwargs) -> ResultT:
        return measure(function, *args, **kwargs)

    return wrapper


def measure(
    function: Callable[..., ResultT],
    *args: Any,
    label: str | None = None,
    **kwargs: Any,
) -> ResultT:
    """Execute a callable, print its elapsed time, and return its result."""
    name = label or getattr(function, "__name__", type(function).__name__)
    start = perf_counter()
    try:
        return function(*args, **kwargs)
    finally:
        elapsed = perf_counter() - start
        print(f"[TIMER] {name}: {elapsed:.6f} s")
