"""Utilities for interacting with redis when writing VS tests.

FIXME:
    - Reference DBs by name rather than ID/socket
    - Add support for ProducerStateTable
"""
from typing import Dict, List
from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result, PollingConfig


class DVSDatabase:
    """DVSDatabase provides access to redis databases on the virtual switch."""

    def __init__(self, db_id: int, connector: str):
        """Initialize a DVSDatabase instance.

        Args:
            db_id: The integer ID used to identify the given database instance in redis.
            connector: The I/O connection used to communicate with
                redis (e.g. UNIX socket, TCP socket, etc.).
        """
        self.db_connection = swsscommon.DBConnector(db_id, connector, 0)

    def create_entry(self, table_name: str, key: str, entry: Dict[str, str]) -> None:
        """Add the mapping {`key` -> `entry`} to the specified table.

        Args:
            table_name: The name of the table to add the entry to.
            key: The key that maps to the entry.
            entry: A set of key-value pairs to be stored.
        """
        table = swsscommon.Table(self.db_connection, table_name)
        formatted_entry = swsscommon.FieldValuePairs(list(entry.items()))
        table.set(key, formatted_entry)

    def set_entry(self, table_name: str, key: str, entry: Dict[str, str]) -> None:
        """Set entry of an existing key in the specified table.

        Args:
            table_name: The name of the table.
            key: The key that needs to be updated.
            entry: A set of key-value pairs to be updated.
        """
        table = swsscommon.Table(self.db_connection, table_name)
        (status, fv_pairs) = table.get(key)

        formatted_entry = swsscommon.FieldValuePairs(list(entry.items()))
        table.set(key, formatted_entry)

        if status:
            for f in [ k for k, v in dict(fv_pairs).items() if k not in entry.keys() ]:
                table.hdel(key, f)

    def update_entry(self, table_name: str, key: str, entry: Dict[str, str]) -> None:
        """Update entry of an existing key in the specified table.

        Args:
            table_name: The name of the table.
            key: The key that needs to be updated.
            entry: A set of key-value pairs to be updated.
        """
        table = swsscommon.Table(self.db_connection, table_name)
        formatted_entry = swsscommon.FieldValuePairs(list(entry.items()))
        table.set(key, formatted_entry)

    def get_entry(self, table_name: str, key: str) -> Dict[str, str]:
        """Get the entry stored at `key` in the specified table.

        Args:
            table_name: The name of the table where the entry is stored.
            key: The key that maps to the entry being retrieved.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """
        table = swsscommon.Table(self.db_connection, table_name)
        (status, fv_pairs) = table.get(key)

        if not status:
            return {}

        return dict(fv_pairs)

    def delete_entry(self, table_name: str, key: str) -> None:
        """Remove the entry stored at `key` in the specified table.

        Args:
            table_name: The name of the table where the entry is being removed.
            key: The key that maps to the entry being removed.
        """
        table = swsscommon.Table(self.db_connection, table_name)
        table._del(key)  # pylint: disable=protected-access

    def delete_field(self, table_name: str, key: str, field: str) -> None:
        """Remove a field from an entry stored at `key` in the specified table.

        Args:
            table_name: The name of the table where the entry is being removed.
            key: The key that maps to the entry being removed.
            field: The field that needs to be removed
        """
        table = swsscommon.Table(self.db_connection, table_name)
        table.hdel(key, field)
        
    def get_keys(self, table_name: str) -> List[str]:
        """Get all of the keys stored in the specified table.

        Args:
            table_name: The name of the table from which to fetch the keys.

        Returns:
            The keys stored in the table. If no keys are found, then an empty List is returned.
        """
        table = swsscommon.Table(self.db_connection, table_name)
        keys = table.getKeys()

        return keys if keys else []

    def wait_for_entry(
        self,
        table_name: str,
        key: str,
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> Dict[str, str]:
        """Wait for the entry stored at `key` in the specified table to exist and retrieve it.

        Args:
            table_name: The name of the table where the entry is stored.
            key: The key that maps to the entry being retrieved.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """

        def access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (bool(fv_pairs), fv_pairs)

        message = failure_message or f'Entry not found: key="{key}", table="{table_name}"'
        _, result = wait_for_result(access_function, polling_config, message)

        return result

    def wait_for_fields(
        self,
        table_name: str,
        key: str,
        expected_fields: List[str],
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> Dict[str, str]:
        """Wait for the entry stored at `key` to have the specified fields and retrieve it.

        This method is useful if you only care about a subset of the fields stored in the
        specified entry.

        Args:
            table_name: The name of the table where the entry is stored.
            key: The key that maps to the entry being checked.
            expected_fields: The fields that we expect to see in the entry.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """

        def access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (all(field in fv_pairs for field in expected_fields), fv_pairs)

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f"Expected fields not found: expected={expected_fields}, received={result}, "
                f'key="{key}", table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_field_match(
        self,
        table_name: str,
        key: str,
        expected_fields: Dict[str, str],
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> Dict[str, str]:
        """Wait for the entry stored at `key` to have the specified field/values and retrieve it.

        This method is useful if you only care about the contents of a subset of the fields stored
        in the specified entry.

        Args:
            table_name: The name of the table where the entry is stored.
            key: The key that maps to the entry being checked.
            expected_fields: The fields and their values we expect to see in the entry.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """

        def access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (
                all(fv_pairs.get(k) == v for k, v in expected_fields.items()),
                fv_pairs,
            )

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f"Expected field/value pairs not found: expected={expected_fields}, "
                f'received={result}, key="{key}", table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_field_negative_match(
        self,
        table_name: str,
        key: str,
        old_fields: Dict[str, str],
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> Dict[str, str]:
        """Wait for the entry stored at `key` to have different field/values than `old_fields`.

        This method is useful if you expect some field to change, but you don't know their exact
        values.

        Args:
            table_name: The name of the table where the entry is stored.
            key: The key that maps to the entry being checked.
            old_fields: The original field/value pairs we expect to change.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """

        def access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (
                all(k in fv_pairs and fv_pairs[k] != v for k, v in old_fields.items()),
                fv_pairs,
            )

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f"Did not expect field/values to match, but they did: provided={old_fields}, "
                f'received={result}, key="{key}", table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_exact_match(
        self,
        table_name: str,
        key: str,
        expected_entry: Dict[str, str],
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> Dict[str, str]:
        """Wait for the entry stored at `key` to match `expected_entry` and retrieve it.

        This method is useful if you care about *all* the fields stored in the specfied entry.

        Args:
            table_name: The name of the table where the entry is stored.
            key: The key that maps to the entry being checked.
            expected_entry: The entry we expect to see.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """

        def access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (fv_pairs == expected_entry, fv_pairs)

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f"Exact match not found: expected={expected_entry}, received={result}, "
                f'key="{key}", table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_deleted_entry(
        self,
        table_name: str,
        key: str,
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> Dict[str, str]:
        """Wait for no entry to exist at `key` in the specified table.

        Args:
            table_name: The name of the table being checked.
            key: The key to be checked.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The entry stored at `key`. If no entry is found, then an empty Dict is returned.
        """

        def access_function():
            fv_pairs = self.get_entry(table_name, key)
            return (not bool(fv_pairs), fv_pairs)

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f'Entry still exists: entry={result}, key="{key}", table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_n_keys(
        self,
        table_name: str,
        num_keys: int,
        wait_at_least_n_keys: bool = False, 
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> List[str]:
        """Wait for the specified number of keys to exist in the table.

        Args:
            table_name: The name of the table from which to fetch the keys.
            num_keys: The expected number of keys to retrieve from the table.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The keys stored in the table. If no keys are found, then an empty List is returned.
        """

        def access_function():
            keys = self.get_keys(table_name)
            if wait_at_least_n_keys:
                return (len(keys) >= num_keys, keys)
            else:
                return (len(keys) == num_keys, keys)

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f"Unexpected number of keys: expected={num_keys}, received={len(result)} "
                f'({result}), table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_matching_keys(
        self,
        table_name: str,
        expected_keys: List[str],
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> List[str]:
        """Wait for the specified keys to exist in the table.

        Args:
            table_name: The name of the table from which to fetch the keys.
            expected_keys: The keys we expect to see in the table.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The keys stored in the table. If no keys are found, then an empty List is returned.
        """

        def access_function():
            keys = self.get_keys(table_name)
            return (all(key in keys for key in expected_keys), keys)

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            message = failure_message or (
                f"Expected keys not found: expected={expected_keys}, received={result}, "
                f'table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    def wait_for_deleted_keys(
        self,
        table_name: str,
        deleted_keys: List[str],
        polling_config: PollingConfig = PollingConfig(),
        failure_message: str = None,
    ) -> List[str]:
        """Wait for the specfied keys to no longer exist in the table.

        Args:
            table_name: The name of the table from which to fetch the keys.
            deleted_keys: The keys we expect to be removed from the table.
            polling_config: The parameters to use to poll the db.
            failure_message: The message to print if the call times out. This will only take effect
                if the PollingConfig is set to strict.

        Returns:
            The keys stored in the table. If no keys are found, then an empty List is returned.
        """

        def access_function():
            keys = self.get_keys(table_name)
            return (all(key not in keys for key in deleted_keys), keys)

        status, result = wait_for_result(
            access_function, self._disable_strict_polling(polling_config)
        )

        if not status:
            expected = [key for key in result if key not in deleted_keys]
            message = failure_message or (
                f"Unexpected keys found: expected={expected}, received={result}, "
                f'table="{table_name}"'
            )
            assert not polling_config.strict, message

        return result

    @staticmethod
    def _disable_strict_polling(polling_config: PollingConfig) -> PollingConfig:
        disabled_config = PollingConfig(
            polling_config.polling_interval, polling_config.timeout, False
        )
        return disabled_config
