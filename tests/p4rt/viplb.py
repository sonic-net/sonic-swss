from swsscommon import swsscommon

import util
import json


class P4RtVIPLBWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT viplb object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_GENERIC_PROGRAMMABLE"
    SAI_ATTR_TYPE = "SAI_GENERIC_PROGRAMMABLE_ATTR_TYPE"
    SAI_ATTR_OBJECT_NAME = "SAI_GENERIC_PROGRAMMABLE_ATTR_OBJECT_NAME"
    SAI_ATTR_ENTRY = "SAI_GENERIC_PROGRAMMABLE_ATTR_ENTRY"

    # default viplb attribute values
    DEFAULT_ACTION = "set_nexthop_id"
    DEFAULT_NEXTHOP_ID = "18"
    DEFAULT_DST = "10.11.12.0/24"

    # attribute fields for viplb object
    NEXTHOP_ID_FIELD = "nexthop_id"

    def generate_app_db_key(self, dst):
        assert self.ip_type is not None
        d = {}
        if self.ip_type == "IPV4":
            d[util.prepend_match_field("ipv4_dst")] = dst
        else:
            d[util.prepend_match_field("ipv6_dst")] = dst
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    def set_ip_type(self, ip_type):
        assert ip_type in ("IPV4", "IPV6")
        self.ip_type = ip_type
        self.TBL_NAME = "EXT_V" + ip_type + "_TABLE"

    # Create entry
    def create_viplb(self, nexthop_id=None, action=None, dst=None):
        action = action or self.DEFAULT_ACTION
        dst = dst or self.DEFAULT_DST
        if action == "set_nexthop_id":
            nexthop_id = nexthop_id or self.DEFAULT_NEXTHOP_ID
            attr_list = [(self.ACTION_FIELD, action),
                         (util.prepend_param_field(self.NEXTHOP_ID_FIELD),
                          nexthop_id)]
        else:
            attr_list = [(self.ACTION_FIELD, action)]
        viplb_key = self.generate_app_db_key(dst)
        self.set_app_db_entry(viplb_key, attr_list)
        return viplb_key, attr_list

    def get_newly_created_programmable_object_oid(self):
        viplb_oid = None
        viplb_entries = util.get_keys(self.asic_db, self.ASIC_DB_TBL_NAME)
        for key in viplb_entries:
            if key not in self._original_entries["{}:{}".format(self.asic_db,
                                                                self.ASIC_DB_TBL_NAME)]:
                viplb_oid = key
                break
        return viplb_oid

    def get_original_appl_db_entries_count(self):
        return len(self._original_entries["%s:%s" % (self.appl_db,
                                                     (self.APP_DB_TBL_NAME + ":"
                                                      + self.TBL_NAME))])

    def get_original_appl_state_db_entries_count(self):
        return len(self._original_entries["%s:%s" % (self.appl_state_db,
                                                     (self.APP_DB_TBL_NAME + ":"
                                                      + self.TBL_NAME))])

