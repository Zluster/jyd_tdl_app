# ruff: noqa: E402


"""Private helpers for translating backend exceptions at Dara boundaries."""

from collections.abc import Callable
from typing import ParamSpec, TypeVar

_Parameters = ParamSpec("_Parameters")
_ReturnType = TypeVar("_ReturnType")
_ErrorType = TypeVar("_ErrorType", bound=Exception)
def wrap_error_as(error_type: type[_ErrorType], message: str, *, catch: type[BaseException] | tuple[type[BaseException], ...]) -> Callable[[Callable[_Parameters, _ReturnType]], Callable[_Parameters, _ReturnType]]:
    """Return a decorator that translates selected exceptions to ``error_type``.

    ``catch`` accepts one exception type or a tuple of exception types. Errors
    already of ``error_type`` are re-raised unchanged.
    """
    ...
