"""Utilities for interacting with SWITCH objects when writing VS tests."""

from typing import Dict, List


class DVSSwitch:
    """Manage switch objects on the virtual switch."""

    ADB_SWITCH = "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH"

    CONFIG_SWITCH_TRIMMING = "SWITCH_TRIMMING"
    KEY_SWITCH_TRIMMING_GLOBAL = "GLOBAL"

    def __init__(self, asic_db, config_db):
        """Create a new DVS switch manager."""
        self.asic_db = asic_db
        self.config_db = config_db

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
            return self.asic_db.get_keys(self.ADB_SWITCH)

        num_keys = len(self.asic_db.default_switch_keys) + expected
        keys = self.asic_db.wait_for_n_keys(self.ADB_SWITCH, num_keys)

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
        self.asic_db.wait_for_field_match(self.ADB_SWITCH, sai_switch_id, sai_qualifiers)
