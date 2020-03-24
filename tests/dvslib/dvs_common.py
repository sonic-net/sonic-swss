"""
    dvs_common contains common infrastructure for writing tests for the
    virtual switch.
"""

import collections
import time

_PollingConfig = collections.namedtuple('PollingConfig', 'polling_interval timeout strict')

class PollingConfig(_PollingConfig):
    """
        PollingConfig provides parameters that are used to control the behavior
        for polling functions.

        Params:
            polling_interval (int): How often to poll, in seconds.
            timeout (int): The maximum amount of time to wait, in seconds.
            strict (bool): If the strict flag is set, reaching the timeout
                will cause tests to fail (e.g. assert False)
    """

    pass

def wait_for_result(polling_function, polling_config):
    """
        wait_for_result will periodically run `polling_function`
        using the parameters described in `polling_config` and return the
        output of the polling function.

        Args:
            polling_config (PollingConfig): The parameters to use to poll
                the db.
            polling_function (Callable[[], (bool, Any)]): The function being
                polled. The function takes no arguments and must return a
                status which indicates if the function was succesful or
                not, as well as some return value.

        Returns:
            Any: The output of the polling function, if it is succesful,
            None otherwise.
    """
    if polling_config.polling_interval == 0:
        iterations = 1
    else:
        iterations = int(polling_config.timeout // polling_config.polling_interval) + 1

    for _ in range(iterations):
        (status, result) = polling_function()

        if status:
            return result

        time.sleep(polling_config.polling_interval)

    if polling_config.strict:
        assert False

    return None
