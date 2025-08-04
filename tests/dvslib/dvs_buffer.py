"""Utilities for interacting with BUFFER objects when writing VS tests."""

from typing import Dict, List


class DVSBuffer:
    """Manage buffer objects on the virtual switch."""

    ASIC_BUFFER_PROFILE = "ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE"
    ASIC_PRIORITY_GROUP = "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP"

    APPL_BUFFER_PROFILE = "BUFFER_PROFILE_TABLE"

    CONFIG_BUFFER_PROFILE = "BUFFER_PROFILE"
    CONFIG_BUFFER_PG = "BUFFER_PG"

    CONFIG_DEVICE_METADATA = "DEVICE_METADATA"
    KEY_DEVICE_METADATA_LOCALHOST = "localhost"

    STATE_BUFFER_MAX_PARAM = "BUFFER_MAX_PARAM_TABLE"
    KEY_BUFFER_MAX_PARAM_GLOBAL = "global"

    COUNTERS_PG_NAME_MAP = "COUNTERS_PG_NAME_MAP"

    def __init__(self, asic_db, app_db, config_db, state_db, counters_db):
        """Create a new DVS buffer manager."""
        self.asic_db = asic_db
        self.app_db = app_db
        self.config_db = config_db
        self.state_db = state_db
        self.counters_db = counters_db

    def get_buffer_pg_keys(
        self,
        port_name: str,
        pg_index: str
    ) -> List[str]:
        """Get priority group buffer keys from CONFIG DB."""
        keyList = []

        keys = self.config_db.get_keys(self.CONFIG_BUFFER_PG)

        for key in keys:
            if port_name in key:
                assert "|" in key, \
                    "Malformed priority group buffer entry: key={}".format(key)
                _, pg = key.split("|")

                if "-" in pg:
                    idx1, idx2 = pg.split("-")
                    if int(idx1) <= int(pg_index) and int(pg_index) <= int(idx2):
                        keyList.append(key)
                else:
                    if int(pg_index) == int(pg):
                        keyList.append(key)

        return keyList

    def get_buffer_pg_value(
        self,
        pg_buffer_key: str,
        pg_buffer_field: str = "profile"
    ) -> str:
        """Get priority group buffer value from CONFIG DB."""
        attr_list = [ pg_buffer_field ]
        fvs = self.config_db.wait_for_fields(self.CONFIG_BUFFER_PG, pg_buffer_key, attr_list)

        return fvs[pg_buffer_field]

    def update_buffer_pg(
        self,
        pg_buffer_key: str,
        pg_buffer_profile: str
    ) -> None:
        """Update priority group in CONFIG DB."""
        attr_dict = {
            "profile": pg_buffer_profile
        }
        self.config_db.update_entry(self.CONFIG_BUFFER_PG, pg_buffer_key, attr_dict)

    def remove_buffer_pg(
        self,
        pg_buffer_key: str
    ) -> None:
        """Remove priority group from CONFIG DB."""
        self.config_db.delete_entry(self.CONFIG_BUFFER_PG, pg_buffer_key)

    def is_dynamic_buffer_model(
        self
    ) -> bool:
        """Checks whether traditional/dynamic buffer model is configured in CONFIG DB."""
        fvs = self.config_db.wait_for_entry(self.CONFIG_DEVICE_METADATA, self.KEY_DEVICE_METADATA_LOCALHOST)
        return fvs.get("buffer_model", "") == "dynamic"

    def wait_for_buffer_profiles(
        self
    ) -> None:
        """Verify all buffer profiles are in ASIC DB."""
        zeroBufferProfileList = [
            "ingress_lossy_pg_zero_profile",
            "ingress_lossy_zero_profile",
            "ingress_lossless_zero_profile",
            "egress_lossy_zero_profile",
            "egress_lossless_zero_profile"
        ]
        bufferProfileList = list(self.config_db.get_keys(self.CONFIG_BUFFER_PROFILE))

        if self.is_dynamic_buffer_model():
            bufferProfileList.extend(zeroBufferProfileList)

        self.app_db.wait_for_matching_keys(self.APPL_BUFFER_PROFILE, bufferProfileList)
        self.asic_db.wait_for_n_keys(self.ASIC_BUFFER_PROFILE, len(bufferProfileList))

    def get_buffer_profile_ids(
        self,
        expected: int = None
    ) -> List[str]:
        """Get all buffer profile ids from ASIC DB."""
        if expected is None:
            return self.asic_db.get_keys(self.ASIC_BUFFER_PROFILE)

        return self.asic_db.wait_for_n_keys(self.ASIC_BUFFER_PROFILE, expected)

    def create_buffer_profile(
        self,
        buffer_profile_name: str,
        qualifiers: Dict[str, str]
    ) -> None:
        """Create buffer profile in CONFIG DB."""
        self.config_db.create_entry(self.CONFIG_BUFFER_PROFILE, buffer_profile_name, qualifiers)

    def remove_buffer_profile(
        self,
        buffer_profile_name: str
    ) -> None:
        """Remove buffer profile from CONFIG DB."""
        self.config_db.delete_entry(self.CONFIG_BUFFER_PROFILE, buffer_profile_name)

    def update_buffer_profile(
        self,
        buffer_profile_name: str,
        qualifiers: Dict[str, str]
    ) -> None:
        """Update buffer profile in CONFIG DB."""
        self.config_db.update_entry(self.CONFIG_BUFFER_PROFILE, buffer_profile_name, qualifiers)

    def verify_buffer_profile(
        self,
        sai_buffer_profile_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that buffer profile object has correct ASIC DB representation."""
        self.asic_db.wait_for_field_match(self.ASIC_BUFFER_PROFILE, sai_buffer_profile_id, sai_qualifiers)

    def update_buffer_mmu(
        self,
        mmu_size: str
    ) -> None:
        """Update buffer MMU size in STATE DB."""
        attr_dict = {
            "mmu_size": mmu_size
        }
        self.state_db.update_entry(self.STATE_BUFFER_MAX_PARAM, self.KEY_BUFFER_MAX_PARAM_GLOBAL, attr_dict)

    def remove_buffer_mmu(
        self
    ) -> None:
        """Remove buffer MMU size from STATE DB."""
        self.state_db.delete_entry(self.STATE_BUFFER_MAX_PARAM, self.KEY_BUFFER_MAX_PARAM_GLOBAL)

    def is_priority_group_exists(
        self,
        port_name: str,
        pg_index: str
    ) -> bool:
        """Verify priority group existence in CONFIG DB."""
        key = "{}|{}".format(port_name, pg_index)
        fvs = self.config_db.get_entry(self.CONFIG_BUFFER_PG, key)

        return bool(fvs)

    def get_priority_group_id(
        self,
        port_name: str,
        pg_index: str
    ) -> str:
        """Get priority group id from COUNTERS DB."""
        field = "{}:{}".format(port_name, pg_index)

        attr_list = [ field ]
        fvs = self.counters_db.wait_for_fields(self.COUNTERS_PG_NAME_MAP, "", attr_list)

        return fvs[field]

    def update_priority_group(
        self,
        port_name: str,
        pg_index: str,
        buffer_profile_name: str
    ) -> None:
        """Update priority group in CONFIG DB."""
        attr_dict = {
            "profile": buffer_profile_name
        }
        key = "{}|{}".format(port_name, pg_index)
        self.config_db.update_entry(self.CONFIG_BUFFER_PG, key, attr_dict)

    def remove_priority_group(
        self,
        port_name: str,
        pg_index: str
    ) -> None:
        """Remove priority group from CONFIG DB."""
        key = "{}|{}".format(port_name, pg_index)
        self.config_db.delete_entry(self.CONFIG_BUFFER_PG, key)

    def verify_priority_group(
        self,
        sai_priority_group_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that priority group object has correct ASIC DB representation."""
        self.asic_db.wait_for_field_match(self.ASIC_PRIORITY_GROUP, sai_priority_group_id, sai_qualifiers)
