from swsscommon import swsscommon
from evpn_tunnel import VxlanTunnel,VxlanEvpnHelper

import time
import pytest

app_df_name = "EVPN_DF_TABLE"
app_shl_name = "EVPN_SPLIT_HORIZON_TABLE"
asic_isolation_name = "ASIC_STATE:SAI_OBJECT_TYPE_ISOLATION_GROUP"
asic_brport_name = "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"
asic_tunnel_name = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
tunnel_src_ip = '10.0.0.2'
portchannel = 'PortChannel0'
vtep = '2.2.2.2'
tunnel_name = 'tunnel_2'
remote_ip_6 = "2.2.2.2"
map_name = 'map_100_100'
vlan_name = "Vlan100"
vni = "100"
nvo_name = "nvo1"


class TestEvpnMultihoming(object):
    def setup_db(self, dvs):
        self.app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.helper = VxlanEvpnHelper()
        self.vxlan_obj = VxlanTunnel()

    def create_portchannel(self, portchannel_name):
        self.helper.create_entry_tbl(
            self.conf_db,
            "PORTCHANNEL", portchannel_name,
            [
                ("admin_status", "up"),
                ("fast_rate", "false"),
                ("lacp_key", "auto"),
                ("min_links", "1"),
                ("mtu", "9100"),
            ],
        )
        time.sleep(2)

    def create_shl_appl_table(self, portchannel_name, vteps):
        self.helper.create_entry_pst(
            self.app_db,
            app_shl_name, portchannel_name,
            [
                ("vteps", vteps),
            ],
        )
        time.sleep(2)

    def create_df_appl_table(self, portchannel_name, is_df):
        is_df_str = "true" if is_df else "false"
        self.helper.create_entry_pst(
            self.app_db,
            app_df_name, portchannel_name,
            [
                ("df", is_df_str),
            ],
        )
        time.sleep(2)

    def create_vlan_member(self, vlan, interface):
        self.helper.create_entry_tbl(
            self.conf_db,
            "VLAN_MEMBER", vlan + "|" + interface,
            [
                ("tagging_mode", "untagged")
            ],
        )
        time.sleep(1)

    def check_shl_appl_table(self, portchannel_name, vteps):
        exp_attr = {
            "vteps": vteps
        }
        self.helper.check_object(self.app_db, app_shl_name, portchannel_name, exp_attr)

    def check_df_appl_table(self, portchannel_name, is_df):
        df_str = "true" if is_df else "false"

        exp_attr = {
            "df": df_str,
        }
        self.helper.check_object(self.app_db, app_df_name, portchannel_name, exp_attr)

    def check_tunnel_ip(self, oid, ip):
        tbl = swsscommon.Table(self.asic_db, asic_tunnel_name)
        (status, fvs) = tbl.get(oid)
        assert status
        values = dict(fvs)
        assert "SAI_TUNNEL_ATTR_ENCAP_DST_IP" in values
        assert values["SAI_TUNNEL_ATTR_ENCAP_DST_IP"] == ip

    def check_df_asic_table(self, is_df):
        entry_created = False

        tbl = swsscommon.Table(self.asic_db, asic_brport_name)
        for key in tbl.getKeys():
            (status, fvs) = tbl.get(key)
            assert status
            values = dict(fvs)

            if "SAI_BRIDGE_PORT_ATTR_NON_DF" not in values:
                continue

            # Note: SAI attirbute value is inverted
            assert values["SAI_BRIDGE_PORT_ATTR_NON_DF"] != is_df

            entry_created = True

        assert entry_created, f"{asic_brport_name}: DF-election entry not present"

    def remove_shf_appl_table(self, portchannel_name):
        self.helper.delete_entry_pst(
            self.app_db,
            app_shl_name, portchannel_name,
        )
        time.sleep(2)

    def remove_df_appl_table(self, portchannel_name):
        self.helper.delete_entry_pst(
            self.app_db,
            app_df_name, portchannel_name,
        )
        time.sleep(2)

    def check_shf_asic_db_entries_deleted(self):
        tbl = swsscommon.Table(self.asic_db, asic_isolation_name)
        entries = tbl.getKeys()
        assert len(entries) == 0, f"{asic_isolation_name} entries not deleted from ASIC_DB"

    #    Test 1 - DF-election
    @pytest.mark.parametrize("is_df", [True, False])
    def test_df_election(self, dvs, testlog, is_df):
        self.setup_db(dvs)

        self.create_portchannel(portchannel)
        self.vxlan_obj.create_vlan1(dvs, vlan_name)
        self.vxlan_obj.check_vlan_obj(dvs, vni)
        self.create_vlan_member(vlan_name, portchannel)

        self.create_df_appl_table(portchannel, is_df)
        self.check_df_appl_table(portchannel, is_df)

        self.check_df_asic_table(is_df)

        self.remove_df_appl_table(portchannel)
        self.check_df_asic_table(is_df)

    #    Test 2 - Split Horizon Filtering
    def test_split_horizon_filtering(self, dvs, testlog):
        self.setup_db(dvs)

        self.create_portchannel(portchannel)
        self.vxlan_obj.create_vlan1(dvs, vlan_name)
        self.vxlan_obj.check_vlan_obj(dvs, vni)
        self.vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, tunnel_src_ip)
        self.vxlan_obj.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        self.vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, vni, vlan_name)
        self.vxlan_obj.create_evpn_remote_vni(dvs, vlan_name, remote_ip_6, vni)
        time.sleep(1)

        self.create_shl_appl_table(portchannel, vtep)
        self.check_shl_appl_table(portchannel, vtep)

        tbl = swsscommon.Table(self.asic_db, asic_isolation_name)
        entries = tbl.getKeys()
        assert len(entries) == 1
        (status, fvs) = tbl.get(entries[0])
        assert status
        for fv in fvs:
            if fv[0] == "SAI_ISOLATION_GROUP_ATTR_TYPE":
                assert fv[1] == "SAI_ISOLATION_GROUP_TYPE_BRIDGE_PORT"

        tbl = swsscommon.Table(self.asic_db, asic_brport_name)
        for key in tbl.getKeys():
            (status, fvs) = tbl.get(key)
            assert status
            values = dict(fvs)
            if "SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP" not in values:
                continue
            assert "SAI_BRIDGE_PORT_ATTR_TUNNEL_ID" in values
            oid = values["SAI_BRIDGE_PORT_ATTR_TUNNEL_ID"]
            self.check_tunnel_ip(oid, vtep)

        self.remove_shf_appl_table(portchannel)
        self.check_shf_asic_db_entries_deleted()
