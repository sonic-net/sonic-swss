"""
    dvs_database contains utilities for interacting with redis when writing
    tests for the virtual switch.
"""
from __future__ import print_function

import time
import collections

from swsscommon import swsscommon


# PollingConfig provides parameters that are used to control polling behavior
# when accessing redis:
# - polling_interval: how often to check for updates in redis
# - timeout: the max amount of time to wait for updates in redis
# - strict: if the strict flag is set, failure to receive updates will cause
#           the polling method to cause tests to fail (e.g. assert False)
PollingConfig = collections.namedtuple('PollingConfig', 'polling_interval timeout strict')


class DVSDatabase(object):
    """
        DVSDatabase provides access to redis databases on the virtual switch.

        By default, database operations are configured to use
        `DEFAULT_POLLING_CONFIG`. Users can specify their own PollingConfig,
        but this shouldn't typically be necessary.
    """
    DEFAULT_POLLING_CONFIG = PollingConfig(polling_interval=0.01, timeout=5, strict=True)

    def __init__(self, db_id, connector):
        """
            Initializes a DVSDatabase instance.

            Args:
                db_id (int): The integer ID used to identify the given database
                    instance in redis.
                connector (str): The I/O connection used to communicate with
                    redis (e.g. unix socket, tcp socket, etc.).
        """

        self.db_connection = swsscommon.DBConnector(db_id, connector, 0)

    def create_entry(self, table_name, key, entry):
        """
            Adds the mapping {`key` -> `entry`} to the specified table.

            Args:
                table_name (str): The name of the table to add the entry to.
                key (str): The key that maps to the entry.
                entry (Dict[str, str]): A set of key-value pairs to be stored.
        """

        table = swsscommon.Table(self.db_connection, table_name)
        formatted_entry = swsscommon.FieldValuePairs(entry.items())
        table.set(key, formatted_entry)

    def wait_for_entry(self, table_name, key,
                       polling_config=DEFAULT_POLLING_CONFIG):
        """
            Gets the entry stored at `key` in the specified table. This method
            will wait for the entry to exist.

            Args:
                table_name (str): The name of the table where the entry is
                    stored.
                key (str): The key that maps to the entry being retrieved.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                Dict[str, str]: The entry stored at `key`. If no entry is found,
                then an empty Dict will be returned.

        """

        access_function = self._get_entry_access_function(table_name, key, True)
        return self._db_poll(polling_config, access_function)

    def delete_entry(self, table_name, key):
        """
            Removes the entry stored at `key` in the specified table.

            Args:
                table_name (str): The name of the table where the entry is
                    being removed.
                key (str): The key that maps to the entry being removed.
        """

        table = swsscommon.Table(self.db_connection, table_name)
        table._del(key)  # pylint: disable=protected-access

    def wait_for_empty_entry(self,
                             table_name,
                             key,
                             polling_config=DEFAULT_POLLING_CONFIG):
        """
            Checks if there is any entry stored at `key` in the specified
            table. This method will wait for the entry to be empty.

            Args:
                table_name (str): The name of the table being checked.
                key (str): The key to be checked.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                bool: True if no entry exists at `key`, False otherwise.
        """

        access_function = self._get_entry_access_function(table_name, key, False)
        return not self._db_poll(polling_config, access_function)

    def wait_for_n_keys(self,
                        table_name,
                        num_keys,
                        polling_config=DEFAULT_POLLING_CONFIG):
        """
            Gets all of the keys stored in the specified table. This method
            will wait for the specified number of keys.

            Args:
                table_name (str): The name of the table from which to fetch
                    the keys.
                num_keys (int): The expected number of keys to retrieve from
                    the table.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                List[str]: The keys stored in the table. If no keys are found,
                then an empty List will be returned.
        """

        access_function = self._get_keys_access_function(table_name, num_keys)
        return self._db_poll(polling_config, access_function)

    def _get_keys_access_function(self, table_name, num_keys):
        """
            Generates an access function to check for `num_keys` in the given
            table and return the list of keys if successful.

            Args:
                table_name (str): The name of the table from which to fetch
                    the keys.
                num_keys (int): The number of keys to check for in the table.
                    If this is set to None, then this function will just return
                    whatever keys are in the table.

            Returns:
                Callable([[], (bool, List[str])]): A function that can be
                called to access the database.

                If `num_keys` keys are found in the given table, or left
                unspecified, then the function will return True along with
                the list of keys that were found. Otherwise, the function will
                return False and some undefined list of keys.
        """

        table = swsscommon.Table(self.db_connection, table_name)

        def _accessor():
            keys = table.getKeys()
            if not keys:
                keys = []

            if not num_keys and num_keys != 0:
                status = True
            else:
                status = len(keys) == num_keys

            return (status, keys)

        return _accessor

    def _get_entry_access_function(self, table_name, key, expect_entry):
        """
            Generates an access function to check for existence of an entry
            at `key` and return it if successful.

            Args:
                table_name (str): The name of the table from which to fetch
                    the entry.
                key (str): The key that maps to the entry being retrieved.
                expect_entry (bool): Whether or not we expect to see an entry
                    at `key`.

            Returns:
                Callable([[], (bool, Dict[str, str])]): A function that can be
                called to access the database.

                If `expect_entry` is set and an entry is found, then the
                function will return True along with the entry that was found.

                If `expect_entry` is not set and no entry is found, then the
                function will return True along with an empty Dict.

                In all other cases, the function will return False with some
                undefined Dict.
        """

        table = swsscommon.Table(self.db_connection, table_name)

        def _accessor():
            (status, fv_pairs) = table.get(key)

            status = expect_entry == status

            if fv_pairs:
                entry = dict(fv_pairs)
            else:
                entry = {}

            return (status, entry)

        return _accessor

    @staticmethod
    def _db_poll(polling_config, access_function):
        """
            _db_poll will periodically run `access_function` on the database
            using the parameters described in `polling_config` and return the
            output of the access function.

            Args:
                polling_config (PollingConfig): The parameters to use to poll
                    the db.
                access_function (Callable[[], (bool, Any)]): The function used
                    for polling the db. Note that the function must return a
                    status which indicates if the function was succesful or
                    not, as well as some return value.

            Returns:
                Any: The output of the access function, if it is succesful,
                None otherwise.
        """
        if polling_config.polling_interval == 0:
            iterations = 1
        else:
            iterations = int(polling_config.timeout // polling_config.polling_interval) + 1

        for _ in range(iterations):
            (status, result) = access_function()

            if status:
                return result

            time.sleep(polling_config.polling_interval)

        if polling_config.strict:
            assert False

        return None
