from swsscommon import swsscommon

import util
import json


class P4RtL3AdmitWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT L3 Admit object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = "FIXED_L3_ADMIT_TABLE"
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_MY_MAC"
    SAI_ATTR_DST_MAC = "SAI_MY_MAC_ATTR_MAC_ADDRESS"
    SAI_ATTR_DST_MAC_MASK = "SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK"
    SAI_ATTR_PORT_ID = "SAI_MY_MAC_ATTR_PORT_ID"
    SAI_ATTR_PRIORITY = "SAI_MY_MAC_ATTR_PRIORITY"

    # attribute fields for l3 admit object in APP DB
    IN_PORT_FIELD = "in_port"
    DST_MAC_FIELD = "dst_mac"
    PRIORITY = "priority"
    L3_ADMIT_ACTION = "admit_to_l3"

    def generate_app_db_key(self, dst_mac, priority, port_id=None):
        d = {}
        d[util.prepend_match_field(self.DST_MAC_FIELD)] = dst_mac
        d[self.PRIORITY] = priority
        if port_id != "" and port_id != None:
             d[util.prepend_match_field(self.IN_PORT_FIELD)] = port_id
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    # create default l3 admit
    def create_l3_admit(
        self, dst_mac, priority, port_id=None
    ):
        attr_list = [
            (self.ACTION_FIELD, self.L3_ADMIT_ACTION),
        ]
        l3_admit_key = self.generate_app_db_key(dst_mac, priority, port_id)
        self.set_app_db_entry(l3_admit_key, attr_list)
        return l3_admit_key, attr_list

    def get_original_appl_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.appl_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_appl_state_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s"
                % (self.appl_state_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_asic_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.asic_db, self.ASIC_DB_TBL_NAME)
            ]
        )

    # Fetch the asic_db_key for the first newly created my mac entry from created
    # my mac in ASIC db. This API should only be used when only one key is
    # expected to be created after original entries.
    # Original my mac entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching asic db key of newly created
    # my mac entries.
    def get_newly_created_asic_db_key(self):
        l3_admit_entries = util.get_keys(self.asic_db, self.ASIC_DB_TBL_NAME)
        for key in l3_admit_entries:
            if (
                key
                not in self._original_entries[
                    "%s:%s" % (self.asic_db, self.ASIC_DB_TBL_NAME)
                ]
            ):
                asic_db_key = key
                break
        return asic_db_key