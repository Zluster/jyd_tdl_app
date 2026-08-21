"""Reusable class registry support."""


from types import GenericAlias



class Registry():
    """Map names to classes that implement a shared contract."""

    @classmethod
    def __class_getitem__(cls, item):
        """Return a subscriptable runtime alias for ``Registry``."""
        return GenericAlias(cls, item)

    def __init__(self, description):
        """Create an empty registry with a human-readable description."""
        self.description = description
        self._classes = {}

    def register(self, name = None, **metadata):
        """Return a decorator that registers a class and its metadata."""

        def decorator(cls):
            key = cls.__name__ if name is None else name
            if key in self._classes:
                raise KeyError(f"{self.description} already contains '{key}'")
            setattr(cls, "__registry_metadata__", metadata)
            self._classes[key] = cls
            return cls

        return decorator

    def get(self, name):
        """Return the class registered under ``name``."""
        return self._classes[name]

    def create(self, name, **kwargs):
        """Create an instance of the class registered under ``name``."""
        return self.get(name)(**kwargs)

    def items(self):
        """Return a dynamic view of registered names and classes."""
        return self._classes.items()
