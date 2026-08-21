"""Private helpers for translating backend exceptions at Dara boundaries."""


from functools import wraps




def wrap_error_as(
    error_type,
    message,
    *,
    catch,
):
    """Return a decorator that translates selected exceptions to ``error_type``.

    ``catch`` accepts one exception type or a tuple of exception types. Errors
    already of ``error_type`` are re-raised unchanged.
    """

    def decorate(
        function,
    ):
        @wraps(function)
        def wrapped(
            *args, **kwargs
        ):
            try:
                return function(*args, **kwargs)
            except error_type:
                raise
            except catch as error:
                raise error_type(message) from error

        return wrapped

    return decorate
