"""Utilities for interacting with SWITCH objects when writing VS tests."""
from typing import Dict, List


class DVSSwitch:
    """Manage switch objects on the virtual switch."""

    ADB_SWITCH = "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH"

    def __init__(self, asic_db):
        """Create a new DVS switch manager."""
        self.asic_db = asic_db

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

    def verify_switch_generic(
        self,
        sai_switch_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that switch object has correct ASIC DB representation.

        Args:
            sai_switch_id: The specific switch id to check in ASIC DB.
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
        """
        entry = self.asic_db.wait_for_entry(self.ADB_SWITCH, sai_switch_id)

        for k, v in entry.items():
            if k == "NULL":
                continue
            elif k in sai_qualifiers:
                if k == "SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM":
                    assert sai_qualifiers[k] == v
                elif k == "SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM":
                    assert sai_qualifiers[k] == v
            else:
                assert False, "Unknown SAI qualifier: key={}, value={}".format(k, v)

    def verify_switch(
        self,
        sai_switch_id: str,
        sai_qualifiers: Dict[str, str],
        strict: bool = False
    ) -> None:
        """Verify that switch object has correct ASIC DB representation.

        Args:
            sai_switch_id: The specific switch id to check in ASIC DB.
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
            strict: Specifies whether verification should be strict
        """
        if strict:
            self.verify_switch_generic(sai_switch_id, sai_qualifiers)
            return

        entry = self.asic_db.wait_for_entry(self.ADB_SWITCH, sai_switch_id)

        attr_dict = {
            **entry,
            **sai_qualifiers
        }

        self.verify_switch_generic(sai_switch_id, attr_dict)
