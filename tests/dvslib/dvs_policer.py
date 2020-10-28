class DVSPolicer(object):
    def __init__(self, adb, cdb):
        self.asic_db = adb
        self.config_db = cdb

    def create_policer(self, name, type="packets", cir="600", cbs="600", mode="sr_tcm", red_action="drop" ):
        policer_entry = {"meter_type": type, "mode": mode,
                         "cir": cir, "cbs": cbs, "red_packet_action": red_action}
        self.config_db.create_entry("POLICER", name, policer_entry)

    def remove_policer(self, name):
        self.config_db.delete_entry("POLICER", name)

    def verify_policer(self, name, expected=1):
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_POLICER", expected)

    def verify_no_policer(self):
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 0)

