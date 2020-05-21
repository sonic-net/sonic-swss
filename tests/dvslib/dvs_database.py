"""
    dvs_database contains utilities for interacting with redis when writing
    tests for the virtual switch.
"""
from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result, PollingConfig


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

    def update_entry(self, table_name, key, entry):
        """
            Updates entries of an existing key in the specified table.

            Args:
                table_name (str): The name of the table.
                key (str): The key that needs to be updated.
                entry (Dict[str, str]): A set of key-value pairs to be updated.
        """

        table = swsscommon.Table(self.db_connection, table_name)
        formatted_entry = swsscommon.FieldValuePairs(entry.items())
        table.set(key, formatted_entry)

    def get_entry(self, table_name, key):
        """
            Gets the entry stored at `key` in the specified table.

            Args:
                table_name (str): The name of the table where the entry is
                    stored.
                key (str): The key that maps to the entry being retrieved.

            Returns:
                Dict[str, str]: The entry stored at `key`. If no entry is found,
                then an empty Dict will be returned.
        """

        table = swsscommon.Table(self.db_connection, table_name)
        (status, fv_pairs) = table.get(key)

        if not status:
            return {}

        return dict(fv_pairs)

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

    def get_keys(self, table_name):
        """
            Gets all of the keys stored in the specified table.

            Args:
                table_name (str): The name of the table from which to fetch
                the keys.

            Returns:
                List[str]: The keys stored in the table. If no keys are found,
                then an empty List will be returned.
        """

        table = swsscommon.Table(self.db_connection, table_name)
        keys = table.getKeys()

        return keys if keys else []

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

        def _access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (bool(fv_pairs), fv_pairs)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Entry not found: key=\"{}\", table=\"{}\"".format(key, table_name)

        return result

    def wait_for_field_match(self,
                             table_name,
                             key,
                             expected_fields,
                             polling_config=DEFAULT_POLLING_CONFIG):
        """
            Checks if the provided fields are contained in the entry stored
            at `key` in the specified table. This method will wait for the
            fields to exist.

            Args:
                table_name (str): The name of the table where the entry is
                    stored.
                key (str): The key that maps to the entry being checked.
                expected_fields (dict): The fields and their values we expect
                    to see in the entry.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                Dict[str, str]: The entry stored at `key`. If no entry is found,
                then an empty Dict will be returned.
        """

        def _access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (all(fv_pairs.get(k) == v for k, v in expected_fields.items()), fv_pairs)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Expected fields not found: expected={}, received={}, \
                key=\"{}\", table=\"{}\"".format(expected_fields, result, key, table_name)

        return result

    def wait_for_exact_match(self,
                             table_name,
                             key,
                             expected_entry,
                             polling_config=DEFAULT_POLLING_CONFIG):
        """
            Checks if the provided entry matches the entry stored at `key`
            in the specified table. This method will wait for the exact entry
            to exist.

            Args:
                table_name (str): The name of the table where the entry is
                    stored.
                key (str): The key that maps to the entry being checked.
                expected_entry (dict): The entry we expect to see.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                Dict[str, str]: The entry stored at `key`. If no entry is found,
                then an empty Dict will be returned.
        """

        def _access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (fv_pairs == expected_entry, fv_pairs)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Exact match not found: expected={}, received={}, \
                key=\"{}\", table=\"{}\"".format(expected_entry, result, key, table_name)

        return result

    def wait_for_deleted_entry(self,
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
                Dict[str, str]: The entry stored at `key`. If no entry is found,
                then an empty Dict will be returned.
        """

        def _access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (not bool(fv_pairs), fv_pairs)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Entry still exists: entry={}, key=\"{}\", table=\"{}\""\
                .format(result, key, table_name)

        return result

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

        def _access_function():
            keys = self.get_keys(table_name)
            return (len(keys) == num_keys, keys)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Unexpected number of keys: expected={}, received={} ({}), table=\"{}\""\
                .format(num_keys, len(result), result, table_name)

        return result

    def wait_for_matching_keys(self,
                               table_name,
                               expected_keys,
                               polling_config=DEFAULT_POLLING_CONFIG):
        """
            Checks if the specified keys exist in the table. This method
            will wait for the keys to exist.

            Args:
                table_name (str): The name of the table from which to fetch
                    the keys.
                expected_keys (List[str]): The keys we expect to see in the
                    table.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                List[str]: The keys stored in the table. If no keys are found,
                then an empty List will be returned.
        """

        def _access_function():
            keys = self.get_keys(table_name)
            return (all(key in keys for key in expected_keys), keys)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Expected keys not found: expected={}, received={}, table=\"{}\""\
                .format(expected_keys, result, table_name)

        return result

    def wait_for_deleted_keys(self,
                              table_name,
                              deleted_keys,
                              polling_config=DEFAULT_POLLING_CONFIG):
        """
            Checks if the specified keys no longer exist in the table. This
            method will wait for the keys to be deleted.

            Args:
                table_name (str): The name of the table from which to fetch
                    the keys.
                deleted_keys (List[str]): The keys we expect to be removed from
                    the table.
                polling_config (PollingConfig): The parameters to use to poll
                    the db.

            Returns:
                List[str]: The keys stored in the table. If no keys are found,
                then an empty List will be returned.
        """

        def _access_function():
            keys = self.get_keys(table_name)
            return (all(key not in keys for key in deleted_keys), keys)

        status, result = wait_for_result(_access_function,
                                         self._disable_strict_polling(polling_config))

        if not status:
            assert not polling_config.strict, \
                "Unexpected keys found: expected={}, received={}, table=\"{}\""\
                .format(deleted_keys, result, table_name)

        return result

    @staticmethod
    def _disable_strict_polling(polling_config):
        disabled_config = PollingConfig(polling_interval=polling_config.polling_interval,
                                        timeout=polling_config.timeout,
                                        strict=False)
        return disabled_config
