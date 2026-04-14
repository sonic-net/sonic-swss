"""Utilities for interacting with SWITCH objects when writing VS tests."""

from typing import Dict, List
from swsscommon import swsscommon


class DVSSwitch:
    """Manage switch objects on the virtual switch."""

    CHANNEL_UNITTEST = "SAI_VS_UNITTEST_CHANNEL"

    ASIC_VIDTORID = "VIDTORID"
    ASIC_SWITCH = "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH"

    CONFIG_SWITCH_TRIMMING = "SWITCH_TRIMMING"
    KEY_SWITCH_TRIMMING_GLOBAL = "GLOBAL"

    COUNTERS_COUNTERS = "COUNTERS"

    def __init__(self, asic_db, config_db, counters_db):
        """Create a new DVS switch manager."""
        self.asic_db = asic_db
        self.config_db = config_db
        self.counters_db = counters_db

    def update_switch_trimming(
        self,
        qualifiers: Dict[str, str]
    ) -> None:
        """Update switch trimming global in CONFIG DB."""
        self.config_db.update_entry(self.CONFIG_SWITCH_TRIMMING, self.KEY_SWITCH_TRIMMING_GLOBAL, qualifiers)

    def get_switch_ids(
        self,
        expected: int = None
    ) -> List[str]:
        """Get all of the switch ids in ASIC DB.

        Args:
            expected: The number of switch ids that are expected to be present in ASIC DB.

        Returns:
            The list of switch ids in ASIC DB.
        """
        if expected is None:
            return self.asic_db.get_keys(self.ASIC_SWITCH)

        num_keys = len(self.asic_db.default_switch_keys) + expected
        keys = self.asic_db.wait_for_n_keys(self.ASIC_SWITCH, num_keys)

        for k in self.asic_db.default_switch_keys:
            assert k in keys

        return [k for k in keys if k not in self.asic_db.default_switch_keys]

    def verify_switch_count(
        self,
        expected: int
    ) -> None:
        """Verify that there are N switch objects in ASIC DB.

        Args:
            expected: The number of switch ids that are expected to be present in ASIC DB.
        """
        self.get_switch_ids(expected)

    def verify_switch(
        self,
        sai_switch_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that switch object has correct ASIC DB representation.

        Args:
            sai_switch_id: The specific switch id to check in ASIC DB.
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
        """
        self.asic_db.wait_for_field_match(self.ASIC_SWITCH, sai_switch_id, sai_qualifiers)

    def set_switch_counter(
        self,
        sai_switch_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Set switch counter value in ASIC DB."""
        attr_list = [ sai_switch_id ]
        fvs = self.asic_db.wait_for_fields(self.ASIC_VIDTORID, "", attr_list)

        ntf = swsscommon.NotificationProducer(self.asic_db.db_connection, self.CHANNEL_UNITTEST)

        # Enable test mode
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "true", fvp)

        # Set switch stats
        key = fvs[sai_switch_id]
        fvp = swsscommon.FieldValuePairs(list(sai_qualifiers.items()))
        ntf.send("set_stats", str(key), fvp)

        # Disable test mode
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", "false", fvp)

    def verify_switch_counter(
        self,
        sai_switch_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that switch counter object has correct COUNTERS DB representation."""
        self.counters_db.wait_for_field_match(self.COUNTERS_COUNTERS, sai_switch_id, sai_qualifiers)
