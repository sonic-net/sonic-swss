"""Utilities for interacting with PBH objects when writing VS tests."""
from typing import Dict, List


class DVSPbh:
    """Manage PBH table/rule/hash and hash field objects on the virtual switch."""

    CDB_PBH_TABLE = "PBH_TABLE"
    CDB_PBH_RULE = "PBH_RULE"
    CDB_PBH_HASH = "PBH_HASH"
    CDB_PBH_HASH_FIELD = "PBH_HASH_FIELD"

    ADB_PBH_HASH = "ASIC_STATE:SAI_OBJECT_TYPE_HASH"

    def __init__(self, asic_db, config_db):
        """Create a new DVS PBH Manager."""
        self.asic_db = asic_db
        self.config_db = config_db

    def create_pbh_table(
        self,
        table_name: str,
        interface_list: List[str],
        description: str = None
    ) -> None:
        """Create PBH table in Config DB."""
        attr_dict = {
            "interface_list": ",".join(interface_list),
            "description": description
        }

        if description is not None:
            attr_dict["description"] = table_name

        self.config_db.create_entry(self.CDB_PBH_TABLE, table_name, attr_dict)

    def remove_pbh_table(
        self,
        table_name: str
    ) -> None:
        """Remove PBH table from Config DB."""
        self.config_db.delete_entry(self.CDB_PBH_TABLE, table_name)

    def create_pbh_rule(
        self,
        table_name: str,
        rule_name: str,
        priority: str,
        qualifiers: Dict[str, str],
        hash_name: str,
        packet_action: str = "SET_ECMP_HASH",
        flow_counter: str = "DISABLED"
    ) -> None:
        """Create PBH rule in Config DB."""
        attr_dict = {
            "priority": priority,
            "hash": hash_name,
            "packet_action": packet_action,
            "flow_counter": flow_counter,
            **qualifiers
        }

        self.config_db.create_entry(self.CDB_PBH_RULE, "{}|{}".format(table_name, rule_name), attr_dict)

    def update_pbh_rule(
        self,
        table_name: str,
        rule_name: str,
        priority: str,
        qualifiers: Dict[str, str],
        hash_name: str,
        packet_action: str = "SET_ECMP_HASH",
        flow_counter: str = "DISABLED"
    ) -> None:
        """Update PBH rule in Config DB."""
        attr_dict = {
            "priority": priority,
            "hash": hash_name,
            "packet_action": packet_action,
            "flow_counter": flow_counter,
            **qualifiers
        }

        self.config_db.set_entry(self.CDB_PBH_RULE, "{}|{}".format(table_name, rule_name), attr_dict)

    def remove_pbh_rule(
        self,
        table_name: str,
        rule_name: str
    ) -> None:
        """Remove PBH rule from Config DB."""
        self.config_db.delete_entry(self.CDB_PBH_RULE, "{}|{}".format(table_name, rule_name))

    def create_pbh_hash(
        self,
        hash_name: str,
        hash_field_list: List[str]
    ) -> None:
        """Create PBH hash in Config DB."""
        attr_dict = {
            "hash_field_list": ",".join(hash_field_list)
        }

        self.config_db.create_entry(self.CDB_PBH_HASH, hash_name, attr_dict)

    def remove_pbh_hash(
        self,
        hash_name: str
    ) -> None:
        """Remove PBH hash from Config DB."""
        self.config_db.delete_entry(self.CDB_PBH_HASH, hash_name)

    def verify_pbh_hash_count(
        self,
        expected: int
    ) -> None:
        """Verify that there are N hash objects in ASIC DB."""
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_HASH", expected)

    def create_pbh_hash_field(
        self,
        hash_field_name: str,
        hash_field: str,
        sequence_id: str,
        ip_mask: str = None
    ) -> None:
        """Create PBH hash field in Config DB."""
        attr_dict = {
            "hash_field": hash_field,
            "sequence_id": sequence_id
        }

        if ip_mask is not None:
            attr_dict["ip_mask"] = ip_mask

        self.config_db.create_entry(self.CDB_PBH_HASH_FIELD, hash_field_name, attr_dict)

    def remove_pbh_hash_field(
        self,
        hash_field_name: str
    ) -> None:
        """Remove PBH hash field from Config DB."""
        self.config_db.delete_entry(self.CDB_PBH_HASH_FIELD, hash_field_name)

    def verify_pbh_hash_field_count(
        self,
        expected: int
    ) -> None:
        """Verify that there are N hash field objects in ASIC DB."""
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_FINE_GRAINED_HASH_FIELD", expected)

    def get_pbh_hash_ids(
        self,
        expected: int
    ) -> List[str]:
        """Get all of the PBH hash IDs in ASIC DB."""
        return self.asic_db.wait_for_n_keys(self.ADB_PBH_HASH, expected)
