# ruff: noqa: E402
"""Reusable class registry support."""

from collections.abc import Callable, ItemsView
from types import GenericAlias
from typing import Generic, TypeVar

_Registered = TypeVar("_Registered")
_RegisteredClass = TypeVar("_RegisteredClass")
class Registry(Generic[_Registered]):
    """Map names to classes that implement a shared contract."""
    def __init__(self, description: str) -> None:
        """Create an empty registry with a human-readable description."""
        ...

    def register(self, name: str | None = ..., **metadata: object) -> Callable[[type[_RegisteredClass]], type[_RegisteredClass]]:
        """Return a decorator that registers a class and its metadata."""
        ...

    def get(self, name: str) -> type[_Registered]:
        """Return the class registered under ``name``."""
        ...

    def create(self, name: str, **kwargs: object) -> _Registered:
        """Create an instance of the class registered under ``name``."""
        ...

    def items(self) -> ItemsView[str, type[_Registered]]:
        """Return a dynamic view of registered names and classes."""
        ...
    @classmethod
    def __class_getitem__(cls, item: object) -> GenericAlias: ...
