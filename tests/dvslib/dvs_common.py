"""Common infrastructure for writing VS tests."""

import collections
import time

from typing import Any, Callable

_PollingConfig = collections.namedtuple('PollingConfig', 'polling_interval timeout strict')


class PollingConfig(_PollingConfig):
    """PollingConfig provides parameters that are used to control polling behavior.

    Attributes:
        polling_interval (int): How often to poll, in seconds.
        timeout (int): The maximum amount of time to wait, in seconds.
        strict (bool): If the strict flag is set, reaching the timeout will cause tests to fail.
    """


def wait_for_result(
    polling_function: Callable[[], (bool, Any)],
    polling_config: PollingConfig,
) -> (bool, Any):
    """Run `polling_function` periodically using the specified `polling_config`.

    Args:
        polling_function: The function being polled. The function cannot take any arguments and
            must return a status which indicates if the function was succesful or not, as well as
            some return value.
        polling_config: The parameters to use to poll the polling function.

    Returns:
        If the polling function succeeds, then this method will return True and the output of the
        polling function.

        If it does not succeed within the provided timeout, it will return False and whatever the
        output of the polling function was on the final attempt.
    """
    if polling_config.polling_interval == 0:
        iterations = 1
    else:
        iterations = int(polling_config.timeout // polling_config.polling_interval) + 1

    for _ in range(iterations):
        status, result = polling_function()

        if status:
            return (True, result)

        time.sleep(polling_config.polling_interval)

    if polling_config.strict:
        assert False, f"Operation timed out after {polling_config.timeout} seconds"

    return (False, result)
