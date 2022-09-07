"""Common infrastructure for writing VS tests."""

import time

from dataclasses import dataclass
from typing import Any, Callable, Tuple


@dataclass
class PollingConfig:
    """Class containing parameters that are used to control polling behavior.

    Attributes:
        polling_interval: How often to poll, in seconds.
        timeout: The maximum amount of time to wait, in seconds.
        strict: If the strict flag is set, reaching the timeout will cause tests to fail.
    """

    polling_interval: float = 0.01
    timeout: float = 20.00
    strict: bool = True

    def iterations(self) -> int:
        """Return the number of iterations needed to poll with the given interval and timeout."""
        return 1 if self.polling_interval == 0 else int(self.timeout // self.polling_interval) + 1


def wait_for_result(
    polling_function: Callable[[], Tuple[bool, Any]],
    polling_config: PollingConfig = PollingConfig(),
    failure_message: str = None,
) -> Tuple[bool, Any]:
    """Run `polling_function` periodically using the specified `polling_config`.

    Args:
        polling_function: The function being polled. The function cannot take any arguments and
            must return a status which indicates if the function was succesful or not, as well as
            some return value.
        polling_config: The parameters to use to poll the polling function.
        failure_message: The message to print if the call times out. This will only take effect
            if the PollingConfig is set to strict.

    Returns:
        If the polling function succeeds, then this method will return True and the output of the
        polling function.

        If it does not succeed within the provided timeout, it will return False and whatever the
        output of the polling function was on the final attempt.
    """
    for _ in range(polling_config.iterations()):
        status, result = polling_function()

        if status:
            return (True, result)

        time.sleep(polling_config.polling_interval)

    if polling_config.strict:
        message = failure_message or f"Operation timed out after {polling_config.timeout} seconds with result {result}"
        assert False, message

    return (False, result)
