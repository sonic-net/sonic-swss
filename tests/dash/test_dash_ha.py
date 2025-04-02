from dash_api.ha_scope_pb2 import *
from dash_api.ha_set_pb2 import *
from dash_api.eni_pb2 import *
from dash_db import *

import time 

scope_mapping = {
    "dpu": Scope.SCOPE_DPU,
    "eni": Scope.SCOPE_ENI
}

role_mapping = {
    "active": Role.HA_SCOPE_ROLE_ACTIVE,
    "dead": Role.HA_SCOPE_ROLE_DEAD,
    "standby": Role.HA_SCOPE_ROLE_STANDBY,
    "switching_to_active": Role.HA_SCOPE_ROLE_SWITCHING_TO_ACTIVE,
    "standalone": Role.HA_SCOPE_ROLE_STANDALONE
}

class TestDashHA(object):

    def test_ha_set(self, dash_db: DashDB):

        # Create HA set
        self.ha_set_id = "e81d4415-8023-4607-b1ac-667b77111469"
        self.local_ip = "1.1.1.1"
        self.peer_ip = "2.2.2.2"
        self.cp_data_channel_port = 1234
        self.data_channel_port = 5678
        self.dp_channel_src_port_min = 1000
        self.dp_channel_src_port_max = 2000
        self.dp_channel_probe_interval_ms = 1 
        self.dp_channel_probe_fail_threshold = 3
        self.scope = "dpu"
        pb = HaSet()
        pb.ha_set_id = self.ha_set_id
        pb.local_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.local_ip)))
        pb.peer_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.peer_ip)))
        pb.cp_data_channel_port = self.cp_data_channel_port
        pb.data_channel_port = self.data_channel_port
        pb.dp_channel_src_port_min = self.dp_channel_src_port_min
        pb.dp_channel_src_port_max = self.dp_channel_src_port_max
        pb.dp_channel_probe_interval_ms = self.dp_channel_probe_interval_ms
        pb.dp_channel_probe_fail_threshold = self.dp_channel_probe_fail_threshold
        pb.scope = scope_mapping[self.scope]
        dash_db.create_ha_set(self.ha_set_id, {"pb": pb.SerializeToString()})

        dash_ha_set_keys = dash_db.wait_for_asic_db_keys(ASIC_DASH_HA_SET_TABLE)
        dash_ha_set_attrs = dash_db.get_asic_db_entry(ASIC_DASH_HA_SET_TABLE, dash_ha_set_keys[0])
        assert_sai_attribute_exists("SAI_HA_SET_ATTR_DP_CHANNEL_PROBE_INTERVAL_MS", dash_ha_set_attrs, self.dp_channel_probe_interval_ms)
        assert_sai_attribute_exists("SAI_HA_SET_ATTR_DP_CHANNEL_PROBE_FAIL_THRESHOLD", dash_ha_set_attrs, self.dp_channel_probe_fail_threshold)

    def test_ha_scope(self, dash_db: DashDB):

        self.scope_id = "e81d4415-8023-4607-b1ac-667b77111469"
        self.ha_role = "dead"
        pb = HaScope()
        pb.ha_scope_id = self.scope_id
        pb.ha_role = role_mapping[self.ha_role]
        dash_db.create_ha_scope(self.scope_id, {"pb": pb.SerializeToString()})

        dash_ha_scope_keys = dash_db.wait_for_asic_db_keys(ASIC_DASH_HA_SCOPE_TABLE)
        dash_ha_scope_attrs = dash_db.get_asic_db_entry(ASIC_DASH_HA_SCOPE_TABLE, dash_ha_scope_keys[0])
        assert_sai_attribute_exists("SAI_HA_SCOPE_ATTR_DASH_HA_ROLE", dash_ha_scope_attrs, role_mapping[self.ha_role])
        assert_sai_attribute_exists("SAI_HA_SCOPE_ATTR_DASH_HA_SET_ID", dash_ha_scope_attrs, self.ha_set_id)
        assert_sai_attribute_exists("SAI_ENI_ATTR_HA_SCOPE_ID", dash_eni_attrs, self.eni_id)

    def test_cleanup(self, dash_db: DashDB):

        self.remove_ha_scope = dash_db.remove_ha_scope(self.scope_id)
        self.remove_ha_set = dash_db.remove_ha_set(self.ha_set_id)