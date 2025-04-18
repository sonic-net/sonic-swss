"""Utilities for interacting with BUFFER objects when writing VS tests."""

from typing import Dict


class DVSBuffer:
    """Manage buffer objects on the virtual switch."""

    ASIC_BUFFER_PROFILE = "ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE"

    CONFIG_BUFFER_PROFILE = "BUFFER_PROFILE"

    STATE_BUFFER_MAX_PARAM = "BUFFER_MAX_PARAM_TABLE"
    KEY_BUFFER_MAX_PARAM_GLOBAL = "global"

    def __init__(self, asic_db, config_db, state_db):
        """Create a new DVS buffer manager."""
        self.asic_db = asic_db
        self.config_db = config_db
        self.state_db = state_db

    def update_buffer_profile(
        self,
        buffer_profile_name: str,
        qualifiers: Dict[str, str]
    ) -> None:
        """Update buffer profile in CONFIG DB."""
        self.config_db.update_entry(self.CONFIG_BUFFER_PROFILE, buffer_profile_name, qualifiers)

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

    def verify_buffer_profile(
        self,
        sai_buffer_profile_id: str,
        sai_qualifiers: Dict[str, str]
    ) -> None:
        """Verify that buffer profile object has correct ASIC DB representation."""
        self.asic_db.wait_for_field_match(self.ASIC_BUFFER_PROFILE, sai_buffer_profile_id, sai_qualifiers)
