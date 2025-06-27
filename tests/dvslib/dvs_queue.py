"""Utilities for interacting with QUEUE objects when writing VS tests."""

from typing import Dict, Union
from swsscommon import swsscommon


class DVSQueue:
    """Manage queue objects on the virtual switch."""

    CHANNEL_UNITTEST = "SAI_VS_UNITTEST_CHANNEL"

    ASIC_VIDTORID = "VIDTORID"
    ASIC_QUEUE = "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE"

    CONFIG_BUFFER_QUEUE = "BUFFER_QUEUE"

    COUNTERS_COUNTERS = "COUNTERS"
    COUNTERS_QUEUE_NAME_MAP = "COUNTERS_QUEUE_NAME_MAP"

    def __init__(self, asic_db, config_db, counters_db):
        """Create a new DVS queue manager."""
        self.asic_db = asic_db
        self.config_db = config_db
        self.counters_db = counters_db

    def get_queue_id(
        self,
        port_name: str,
        queue_index: str
    ) -> str:
        """Get queue id from COUNTERS DB."""
        field = "{}:{}".format(port_name, queue_index)

        attr_list = [ field ]
        fvs = self.counters_db.wait_for_fields(self.COUNTERS_QUEUE_NAME_MAP, "", attr_list)

        return fvs[field]

    def get_queue_buffer_profile_id(
        self,
        port_name: str,
        queue_index: str
    ) -> str:
        """Get queue buffer profile id from ASIC DB."""
        field = "SAI_QUEUE_ATTR_BUFFER_PROFILE_ID"

        sai_queue_id = self.get_queue_id(port_name, queue_index)
        attr_list = [ field ]
        fvs = self.asic_db.wait_for_fields(self.ASIC_QUEUE, sai_queue_id, attr_list)

        return fvs[field]

    def get_queue_buffer_profile_name(
        self,
        port_name: str,
        queue_index: str
    ) -> str:
        """Get queue buffer profile name from CONFIG DB."""
        def get_buffer_queue_key(port: str, idx: str) -> Union[str, None]:
            keys = self.config_db.get_keys(self.CONFIG_BUFFER_QUEUE)

            for key in keys:
                if port in key:
                    assert "|" in key, \
                        "Malformed queue buffer entry: key={}".format(key)
                    _, queue = key.split("|")

                    if "-" in queue:
                        idx1, idx2 = queue.split("-")
                        if int(idx1) <= int(idx) and int(idx) <= int(idx2):
                            return key
                    else:
                        if int(idx) == int(queue):
                            return key

            return None

        key = get_buffer_queue_key(port_name, queue_index)
        assert key is not None, \
            "Queue buffer profile name doesn't exist: port={}, queue={}".format(port_name, queue_index)

        field = "profile"

        attr_list = [ field ]
        fvs = self.config_db.wait_for_fields(self.CONFIG_BUFFER_QUEUE, key, attr_list)

        return fvs[field]

    def set_queue_counter(
        self,
        sai_queue_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Set queue counter value in ASIC DB."""
        attr_list = [ sai_queue_id ]
        fvs = self.asic_db.wait_for_fields(self.ASIC_VIDTORID, "", attr_list)

        ntf = swsscommon.NotificationProducer(self.asic_db.db_connection, self.CHANNEL_UNITTEST)

        # Enable test mode
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "true", fvp)

        # Set queue stats
        key = fvs[sai_queue_id]
        fvp = swsscommon.FieldValuePairs(list(sai_qualifiers.items()))
        ntf.send("set_stats", str(key), fvp)

        # Disable test mode
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "false", fvp)

    def verify_queue_counter(
        self,
        sai_queue_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that queue counter object has correct COUNTERS DB representation."""
        self.counters_db.wait_for_field_match(self.COUNTERS_COUNTERS, sai_queue_id, sai_qualifiers)
