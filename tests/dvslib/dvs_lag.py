import json

class DVSLag(object):
    def __init__(self, adb, cdb, dvs):
        self.asic_db = adb
        self.config_db = cdb
        self.dvs = dvs

    def create_port_channel(self, lag_id, admin_status="up", mtu="1500", fast_rate=False):
        lag = "PortChannel{}".format(lag_id)
        lag_entry = {"admin_status": admin_status, "mtu": mtu, "fast_rate": str(fast_rate).lower()}
        self.config_db.create_entry("PORTCHANNEL", lag, lag_entry)

    def remove_port_channel(self, lag_id):
        lag = "PortChannel{}".format(lag_id)
        self.config_db.delete_entry("PORTCHANNEL", lag)

    def create_port_channel_member(self, lag_id, interface):
        member = "PortChannel{}|{}".format(lag_id, interface)
        member_entry = {"NULL": "NULL"}
        self.config_db.create_entry("PORTCHANNEL_MEMBER", member, member_entry)

    def remove_port_channel_member(self, lag_id, interface):
        member = "PortChannel{}|{}".format(lag_id, interface)
        self.config_db.delete_entry("PORTCHANNEL_MEMBER", member)

    def get_and_verify_port_channel_members(self, expected_num):
        return self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", expected_num)

    def get_and_verify_port_channel(self, expected_num):
        return self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", expected_num)

    def dump_portchannel(self, lag_id):
        lag = "PortChannel{}".format(lag_id)
        output = self.dvs.runcmd("teamdctl {} state dump".format(lag))[1]
        port_state_dump = json.loads(output)
        return port_state_dump

    def get_and_verify_port_channel_fast_rate(self, lag_id, fast_rate):
        assert self.dump_portchannel(lag_id)["runner"]["fast_rate"] == fast_rate

