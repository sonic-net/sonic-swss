class DVSLag(object):
    def __init__(self, cdb):
        self.config_db = cdb

    def create_port_channel(self, lag_id, admin_status="up", mtu="1500"):
        lag = "PortChannel{}".format(lag_id)
        lag_entry = {"admin_status": admin_status, "mtu": mtu}
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
