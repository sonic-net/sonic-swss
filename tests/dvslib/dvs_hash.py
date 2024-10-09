"""Utilities for interacting with HASH objects when writing VS tests."""
from typing import Dict, List
import time


class DVSHash:
    """Manage hash objects on the virtual switch."""

    CDB_SWITCH_HASH = "SWITCH_HASH"
    KEY_SWITCH_HASH_GLOBAL = "GLOBAL"

    ADB_HASH = "ASIC_STATE:SAI_OBJECT_TYPE_HASH"

    def __init__(self, asic_db, config_db):
        """Create a new DVS hash manager."""
        self.asic_db = asic_db
        self.config_db = config_db

    def update_switch_hash(
        self,
        qualifiers: Dict[str, str]
    ) -> None:
        """Update switch hash global in Config DB."""
        self.config_db.update_entry(self.CDB_SWITCH_HASH, self.KEY_SWITCH_HASH_GLOBAL, qualifiers)
        time.sleep(1)

    def get_hash_ids(
        self,
        expected: int = None
    ) -> List[str]:
        """Get all of the hash ids in ASIC DB.

        Args:
            expected: The number of hash ids that are expected to be present in ASIC DB.

        Returns:
            The list of hash ids in ASIC DB.
        """
        if expected is None:
            return self.asic_db.get_keys(self.ADB_HASH)

        num_keys = len(self.asic_db.default_hash_keys) + expected
        keys = self.asic_db.wait_for_n_keys(self.ADB_HASH, num_keys)

        for k in self.asic_db.default_hash_keys:
            assert k in keys

        return [k for k in keys if k not in self.asic_db.default_hash_keys]

    def verify_hash_count(
        self,
        expected: int
    ) -> None:
        """Verify that there are N hash objects in ASIC DB.

        Args:
            expected: The number of hash ids that are expected to be present in ASIC DB.
        """
        self.get_hash_ids(expected)

    def verify_hash_generic(
        self,
        sai_hash_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that hash object has correct ASIC DB representation.

        Args:
            sai_hash_id: The specific hash id to check in ASIC DB.
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
        """
        entry = self.asic_db.wait_for_entry(self.ADB_HASH, sai_hash_id)

        for k, v in entry.items():
            if k == "NULL":
                continue
            elif k in sai_qualifiers:
                if k == "SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST":
                    hfList = v[v.index(":")+1:].split(",")
                    assert set(sai_qualifiers[k]) == set(hfList)
            else:
                assert False, "Unknown SAI qualifier: key={}, value={}".format(k, v)
