from swsscommon import swsscommon
import time
import json
from pytest import *
       
class VxlanEvpnHelper(object):
    def create_entry(self, tbl, key, pairs):
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(key, fvs)
        time.sleep(1)

    def create_entry_tbl(self, db, table, key, pairs):
        tbl = swsscommon.Table(db, table)
        self.create_entry(tbl, key, pairs)

    def delete_entry_tbl(self, db, table, key):
        tbl = swsscommon.Table(db, table)
        tbl._del(key)
        time.sleep(1)

    def create_entry_pst(self, db, table,  key, pairs):
        tbl = swsscommon.ProducerStateTable(db, table)
        self.create_entry(tbl, key, pairs)

    def delete_entry_pst(self, db, table, key):
        tbl = swsscommon.ProducerStateTable(db, table)
        tbl._del(key)
        time.sleep(1)

    def how_many_entries_exist(self, db, table):
        tbl =  swsscommon.Table(db, table)
        return len(tbl.getKeys())

    def get_exist_entries(self, dvs, table):
        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl =  swsscommon.Table(db, table)
        return set(tbl.getKeys())

    def get_created_entry(self, db, table, existed_entries):
        tbl =  swsscommon.Table(db, table)
        entries = set(tbl.getKeys())
        new_entries = list(entries - existed_entries)
        assert len(new_entries) == 1, "Wrong number of created entries."
        return new_entries[0]

    def get_created_entries(self, db, table, existed_entries, count):
        tbl =  swsscommon.Table(db, table)
        entries = set(tbl.getKeys())
        new_entries = list(entries - existed_entries)
        assert len(new_entries) == count, "Wrong number of created entries."
        new_entries.sort()
        return new_entries

    def get_deleted_entries(self, db, table, existed_entries, count):
        tbl =  swsscommon.Table(db, table)
        entries = set(tbl.getKeys())
        old_entries = list(existed_entries - entries)
        assert len(old_entries) == count, "Wrong number of deleted entries."
        old_entries.sort()
        return old_entries

    def check_object(self, db, table, key, expected_attributes):
        tbl =  swsscommon.Table(db, table)
        keys = tbl.getKeys()
        assert key in keys, "The desired key is not presented"

        status, fvs = tbl.get(key)
        assert status, "Got an error when get a key"

        assert len(fvs) >= len(expected_attributes), "Incorrect attributes"

        for name, value in fvs:
            if name in expected_attributes:
                assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])

    def get_key_with_attr(self, db, table, expected_attributes ):
        tbl =  swsscommon.Table(db, table)
        keys = tbl.getKeys()
        retkey = list()

        for key in keys:
            status, fvs = tbl.get(key)
            assert status, "Got an error when get a key"

            if len(fvs) < len(expected_attributes):
                continue

            num_match = 0
            for name, value in fvs:
                if name in expected_attributes:
                    if expected_attributes[name] == value:
                         num_match += 1
            if num_match == len(expected_attributes):
                retkey.append(key)

        return retkey

    def check_deleted_object(self, db, table, key):
        tbl =  swsscommon.Table(db, table)
        keys = tbl.getKeys()
        assert key not in keys, "The desired key is not removed"

class VxlanTunnel(object):

    ASIC_TUNNEL_TABLE       = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_MAP         = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP"
    ASIC_TUNNEL_MAP_ENTRY   = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY"
    ASIC_TUNNEL_TERM_ENTRY  = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_BRIDGE_PORT        = "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"
    ASIC_VRF_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
    ASIC_RIF_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_ROUTE_ENTRY        = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    ASIC_NEXT_HOP           = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_NEXT_HOP_GRP       = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
    ASIC_NEXT_HOP_GRP_MEMBERS = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"

    tunnel_map_ids           = set()
    tunnel_map_entry_ids     = set()
    tunnel_map_vrf_entry_ids = set()
    tunnel_ids               = set()
    tunnel_term_ids          = set()
    bridgeport_ids           = set()
    tunnel_map_map           = {}
    tunnel                   = {}
    tunnel_appdb             = {}
    tunnel_term              = {}
    map_entry_map            = {}
    dip_tunnel_map           = {}
    dip_tun_state_map        = {}
    diptunterm_map           = {}
    bridgeport_map           = {}
    vlan_id_map              = {}
    vlan_member_map          = {}
    l2mcgroup_member_map     = {}
    l2mcgroup_map            = {}  
    vr_map                   = {}
    vnet_vr_ids              = set()
    nh_ids                   = {}
    nh_grp_id                = set()
    nh_grp_member_id         = set()
    route_id                 = {}
    helper                   = None
    switch_mac               = None


    def __init__(self):
        self.helper = VxlanEvpnHelper()

    def create_evpn_nvo(self, dvs, nvoname, tnl_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        attrs = [
            ("source_vtep", tnl_name),
        ]

        # create the VXLAN tunnel Term entry in Config DB
        self.helper.create_entry_tbl(
            conf_db,
            "VXLAN_EVPN_NVO", nvoname,
            attrs,
        )

    def remove_evpn_nvo(self, dvs, nvoname):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.helper.delete_entry_tbl(conf_db,"VXLAN_EVPN_NVO", nvoname)

    def create_vxlan_tunnel(self, dvs, name, src_ip, dst_ip = '0.0.0.0', skip_dst_ip=True):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        attrs = [
                ("src_ip", src_ip),
        ]

        if not skip_dst_ip:
            attrs.append(("dst_ip", dst_ip))

        # create the VXLAN tunnel Term entry in Config DB
        self.helper.create_entry_tbl(
            conf_db,
            "VXLAN_TUNNEL", name,
            attrs,
        )

    def create_vxlan_tunnel_map(self, dvs, tnl_name, map_name, vni_id, vlan_id):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        attrs = [
                ("vni", vni_id),
                ("vlan", vlan_id),
        ]

        # create the VXLAN tunnel Term entry in Config DB
        self.helper.create_entry_tbl(
            conf_db,
            "VXLAN_TUNNEL_MAP", "%s|%s" % (tnl_name, map_name),
            attrs,
        )

    def create_evpn_remote_vni(self, dvs, vlan_id, remote_vtep, vnid):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.helper.create_entry_pst(
            app_db,
            "VXLAN_REMOTE_VNI_TABLE", "%s:%s" % (vlan_id, remote_vtep),
            [
                ("vni", vnid),
            ],
        )
        time.sleep(2)

    def remove_vxlan_tunnel(self, dvs, tnl_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        # create the VXLAN tunnel Term entry in Config DB
        self.helper.delete_entry_tbl(
            conf_db,
            "VXLAN_TUNNEL", tnl_name
        )

    def remove_vxlan_tunnel_map(self, dvs, tnl_name, map_name,vni_id, vlan_id):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        # Remove the VXLAN tunnel map entry in Config DB
        self.helper.delete_entry_tbl(
            conf_db,
            "VXLAN_TUNNEL_MAP", "%s|%s" % (tnl_name, map_name)
        )

    def remove_evpn_remote_vni(self, dvs, vlan_id, remote_vtep ):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.helper.delete_entry_pst(
            app_db,
            "VXLAN_REMOTE_VNI_TABLE", "%s:%s" % (vlan_id, remote_vtep),
        )
        time.sleep(2)

    def create_vxlan_vrf_tunnel_map(self, dvs, vrfname, vni_id):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        attrs = [
                ("vni", vni_id),
        ]

        # create the VXLAN VRF tunnel Term entry in Config DB
        self.helper.create_entry_tbl(
            conf_db,
            "VRF", vrfname,
            attrs,
        )

    def remove_vxlan_vrf_tunnel_map(self, dvs, vrfname):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        attrs = [
                ("vni", "0"),
        ]

        # remove the VXLAN VRF tunnel Term entry in Config DB
        self.helper.create_entry_tbl(
            conf_db,
            "VRF", vrfname,
            attrs,
        )

    def create_vlan1(self, dvs, vlan_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        vlan_id = vlan_name[4:]

        # create vlan
        self.helper.create_entry_tbl(
            conf_db,
            "VLAN", vlan_name,
            [
              ("vlanid", vlan_id),
            ],
    )

    def create_vrf_route(self, dvs, prefix, vrf_name, endpoint, ifname, mac="", vni=0):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        attrs = [
                ("nexthop", endpoint),
                ("ifname", ifname),
        ]

        if vni:
            attrs.append(('vni_label', vni))

        if mac:
            attrs.append(('router_mac', mac))

        self.helper.create_entry_pst(
            app_db,
            "ROUTE_TABLE", "%s:%s" % (vrf_name, prefix),
            attrs,
        )

        time.sleep(2)

    def delete_vrf_route(self, dvs, prefix, vrf_name):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        self.helper.delete_entry_pst(app_db, "ROUTE_TABLE", "%s:%s" % (vrf_name, prefix))

        time.sleep(2)

    def create_vrf_route_ecmp(self, dvs, prefix, vrf_name, ecmp_nexthop_attributes):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        self.helper.create_entry_pst(
            app_db,
            "ROUTE_TABLE",  "%s:%s" % (vrf_name, prefix),
            ecmp_nexthop_attributes,
        )

        time.sleep(2)

    def create_vlan(self, dvs, vlan_name, vlan_ids):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        vlan_id = vlan_name[4:]

        # create vlan
        self.helper.create_entry_tbl(
            conf_db,
            "VLAN",  vlan_name,
            [
              ("vlanid", vlan_id),
           ],
        )

        time.sleep(1)

        vlan_oid = self.helper.get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan_ids)

        self.helper.check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan_oid,
                        {
                            "SAI_VLAN_ATTR_VLAN_ID": vlan_id,
                        }
                    )

        return vlan_oid

    def remove_vlan(self, dvs, vlan):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(conf_db, "VLAN")
        tbl._del("Vlan" + vlan)
        time.sleep(1)

    def create_vlan_member(self, dvs, vlan, interface, tagging_mode="untagged"):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(conf_db, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", tagging_mode)])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, dvs, vlan, interface):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(conf_db, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan + "|" + interface)
        time.sleep(1)


    def create_vlan_interface(self, dvs, vlan_name, ifname, vrf_name, ipaddr):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        # create a vlan member in config db
        self.helper.create_entry_tbl(
            conf_db,
            "VLAN_MEMBER",  "%s|%s" % (vlan_name, ifname),
            [
              ("tagging_mode", "untagged"),
            ],
        )

        time.sleep(1)

        # create vlan interface in config db
        self.helper.create_entry_tbl(
            conf_db,
            "VLAN_INTERFACE",  vlan_name,
            [
              ("vrf_name", vrf_name),
            ],
        )

        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.helper.create_entry_pst(
            app_db,
            "INTF_TABLE", vlan_name,
            [
                ("vrf_name", vrf_name),
            ],
        )
        time.sleep(2)

        self.helper.create_entry_tbl(
            conf_db,
            "VLAN_INTERFACE",  "%s|%s" % (vlan_name, ipaddr),
            [
              ("family", "IPv4"),
            ],
        )

        time.sleep(2)

    def delete_vlan_interface(self, dvs, ifname, ipaddr):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        self.helper.delete_entry_tbl(conf_db, "VLAN_INTERFACE", "%s|%s" % (ifname, ipaddr))
        time.sleep(2)

        self.helper.delete_entry_tbl(conf_db, "VLAN_INTERFACE", ifname)
        time.sleep(2)

    def get_switch_mac(self, dvs):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH')

        entries = tbl.getKeys()
        mac = None
        for entry in entries:
            status, fvs = tbl.get(entry)
            assert status, "Got an error when get a key"
            for key, value in fvs:
                if key == 'SAI_SWITCH_ATTR_SRC_MAC_ADDRESS':
                    mac = value
                    break
            else:
                assert False, 'Don\'t found switch mac'

        return mac

    def fetch_exist_entries(self, dvs):
        self.tunnel_ids = self.helper.get_exist_entries(dvs, self.ASIC_TUNNEL_TABLE)
        self.tunnel_map_ids = self.helper.get_exist_entries(dvs, self.ASIC_TUNNEL_MAP)
        self.tunnel_map_entry_ids = self.helper.get_exist_entries(dvs, self.ASIC_TUNNEL_MAP_ENTRY)
        self.tunnel_term_ids = self.helper.get_exist_entries(dvs, self.ASIC_TUNNEL_TERM_ENTRY)
        self.bridgeport_ids = self.helper.get_exist_entries(dvs, self.ASIC_BRIDGE_PORT)
        self.vnet_vr_ids = self.helper.get_exist_entries(dvs, self.ASIC_VRF_TABLE)
        self.rifs = self.helper.get_exist_entries(dvs, self.ASIC_RIF_TABLE)
        self.routes = self.helper.get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
        self.nhops = self.helper.get_exist_entries(dvs, self.ASIC_NEXT_HOP)
        self.nhop_grp = self.helper.get_exist_entries(dvs, self.ASIC_NEXT_HOP_GRP)
        self.nhop_grp_members = self.helper.get_exist_entries(dvs, self.ASIC_NEXT_HOP_GRP_MEMBERS)

        if self.switch_mac is None:
            self.switch_mac = self.get_switch_mac(dvs)

    def check_vxlan_tunnel_map_entry_delete(self, dvs, tunnel_name, vidlist, vnilist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP_ENTRY)

        for x in range(len(vidlist)):
            status, fvs = tbl.get(self.map_entry_map[tunnel_name + vidlist[x]])
            assert status == False, "SIP Tunnel Map entry not deleted"
            iplinkcmd = "ip link show type vxlan dev " + tunnel_name + "-" + vidlist[x]
            (exitcode, out) = dvs.runcmd(iplinkcmd)
            assert exitcode != 0, "Kernel device not deleted"

    def check_vxlan_tunnel_map_entry(self, dvs, tunnel_name, vidlist, vnidlist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP_ENTRY)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': self.tunnel_map_map[tunnel_name][0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(len(vidlist)):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, expected_attributes_1)
            assert len(ret) > 0, "SIP TunnelMap entry not created"
            assert len(ret) == 1, "More than 1 SIP TunnMapEntry created"
            self.map_entry_map[tunnel_name + vidlist[x]] = ret[0]
            iplinkcmd = "ip link show type vxlan dev " + tunnel_name + "-" + vidlist[x]
            (exitcode, out) = dvs.runcmd(iplinkcmd)
            assert exitcode == 0, "Kernel device not created"

    def check_vxlan_tunnel_map_entry_removed(self, dvs, tunnel_name, vidlist, vnidlist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': self.tunnel_map_map[tunnel_name][0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(len(vidlist)):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, expected_attributes_1)
            assert len(ret) == 0, "SIP TunnelMap entry not removed"

    def check_vxlan_tunnel_state_table(self, dvs, src_ip, dst_ip):
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        expected_state_attributes = {
            'src_ip': src_ip,
            'dst_ip': dst_ip,
            'tnl_src': 'EVPN',
        }

        ret = self.helper.get_key_with_attr(state_db, 'VXLAN_TUNNEL_TABLE', expected_state_attributes)
        assert len(ret) > 0, "Tunnel Statetable entry not created"
        assert len(ret) == 1, "More than 1 Tunn statetable entry created"
        self.dip_tun_state_map[dst_ip] = ret[0]

    def check_vxlan_tunnel_state_table_delete(self, dvs, dst_ip):
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(state_db, 'VXLAN_TUNNEL_TABLE')
        status, fvs = tbl.get(self.dip_tun_state_map[dst_ip])
        assert status == False, "State Table entry not deleted"


    def check_vxlan_sip_tunnel_delete(self, dvs, tunnel_name, sip, ignore_bp = True):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(app_db, "VXLAN_TUNNEL_TABLE")
        status, fvs = tbl.get(self.tunnel_appdb[tunnel_name])
        assert status == False, "SIP Tunnel entry not deleted from APP_DB"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TERM_ENTRY)
        status, fvs = tbl.get(self.tunnel_term[tunnel_name])
        assert status == False, "SIP Tunnel Term entry not deleted from ASIC_DB"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TABLE)
        status, fvs = tbl.get(self.tunnel[tunnel_name])
        assert status == False, "SIP Tunnel entry not deleted from ASIC_DB"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP)
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][0])
        assert status == False, "SIP Tunnel mapper0 not deleted from ASIC_DB"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][1])
        assert status == False, "SIP Tunnel mapper1 not deleted from ASIC_DB"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][2])
        assert status == False, "SIP Tunnel mapper2 not deleted from ASIC_DB"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][3])
        assert status == False, "SIP Tunnel mapper3 not deleted from ASIC_DB"

        tbl = swsscommon.Table(state_db, 'VXLAN_TUNNEL_TABLE')
        status, fvs = tbl.get(tunnel_name)
        assert status == False, "State Table entry not deleted"

        if not ignore_bp:
            tbl = swsscommon.Table(asic_db, self.ASIC_BRIDGE_PORT)
            status, fvs = tbl.get(self.bridgeport_map[sip])
            assert status == False, "Tunnel bridgeport entry not deleted"

    def check_vxlan_sip_tunnel(self, dvs, tunnel_name, src_ip, vidlist, vnidlist, 
                               dst_ip = '0.0.0.0', skip_dst_ip = 'True', ignore_bp = True,
                               tunnel_map_entry_count = 3):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        tunnel_map_id  = self.helper.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 4)
        tunnel_id      = self.helper.get_created_entry(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids)
        tunnel_term_id = self.helper.get_created_entry(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids)

        # check that the vxlan tunnel termination are there
        assert self.helper.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP) == (len(self.tunnel_map_ids) + 4), "The TUNNEL_MAP wasn't created"
        assert self.helper.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + tunnel_map_entry_count), "The TUNNEL_MAP_ENTRY is created"
        assert self.helper.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TABLE) == (len(self.tunnel_ids) + 1), "The TUNNEL wasn't created"
        assert self.helper.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TERM_ENTRY) == (len(self.tunnel_term_ids) + 1), "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

        expected_attributes_1 = {}
        expected_attributes_1['SAI_TUNNEL_MAP_ATTR_TYPE'] = 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID'
        ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP, expected_attributes_1)
        assert len(ret) == 1, "Unexpected number of tunnel maps created for type SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID"

        expected_attributes_1['SAI_TUNNEL_MAP_ATTR_TYPE'] = 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI'
        ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP, expected_attributes_1)
        assert len(ret) == 1, "Unexpected number of tunnel maps created for type SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI"


        decapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[2]
        encapstr = '2:' + tunnel_map_id[1] + ',' + tunnel_map_id[3]
                        #'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': loopback_id,

        self.helper.check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id,
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_PEER_MODE': 'SAI_TUNNEL_PEER_MODE_P2MP',
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                        'SAI_TUNNEL_ATTR_ENCAP_TTL_MODE': 'SAI_TUNNEL_TTL_MODE_PIPE_MODEL',
                        'SAI_TUNNEL_ATTR_ENCAP_TTL_VAL': '255',
                    }
                )

        expected_attributes = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
        }

        if not skip_dst_ip:
           expected_attributes['SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP'] = dst_ip
           expected_attributes['SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE'] = 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P'

        self.helper.check_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id, expected_attributes)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(len(vidlist)):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, expected_attributes_1)
            assert len(ret) == 1, "Unexpected number of tunnel map entries created for VLAN to VNI mapping"

        expected_siptnl_attributes = {
            'src_ip': src_ip,
        }

        if not skip_dst_ip:
           expected_siptnl_attributes['dst_ip'] =  dst_ip

        ret = self.helper.get_key_with_attr(app_db, "VXLAN_TUNNEL_TABLE", expected_siptnl_attributes)
        assert len(ret) > 0, "SIP Tunnel entry not created in APPDB"
        assert len(ret) == 1, "More than 1 Tunn statetable entry created"
        self.tunnel_appdb[tunnel_name] = ret[0]

        if not ignore_bp:
            expected_bridgeport_attributes = {
            'SAI_BRIDGE_PORT_ATTR_TYPE': 'SAI_BRIDGE_PORT_TYPE_TUNNEL',
            'SAI_BRIDGE_PORT_ATTR_TUNNEL_ID': tunnel_id,
            'SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE': 'SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE',
            'SAI_BRIDGE_PORT_ATTR_ADMIN_STATE': 'true',
            }
            ret = self.helper.get_key_with_attr(asic_db, self.ASIC_BRIDGE_PORT, expected_bridgeport_attributes)
            assert len(ret) > 0, "Bridgeport entry not created"
            assert len(ret) == 1, "More than 1 bridgeport entry created"
            self.bridgeport_map[src_ip] = ret[0]

        self.tunnel_map_ids.update(tunnel_map_id)
        self.tunnel_ids.add(tunnel_id)
        self.tunnel_term_ids.add(tunnel_term_id)
        self.tunnel_map_map[tunnel_name] = tunnel_map_id
        self.tunnel[tunnel_name] = tunnel_id
        self.tunnel_term[tunnel_name] = tunnel_term_id

    def check_vxlan_dip_tunnel_delete(self, dvs, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        self.check_vxlan_tunnel_state_table_delete(dvs, dip)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TABLE)
        status, fvs = tbl.get(self.dip_tunnel_map[dip])
        assert status == False, "Tunnel entry not deleted"

        tbl = swsscommon.Table(asic_db, self.ASIC_BRIDGE_PORT)
        status, fvs = tbl.get(self.bridgeport_map[dip])
        assert status == False, "Tunnel bridgeport entry not deleted"

    def check_vxlan_dip_tunnel(self, dvs, vtep_name, src_ip, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        self.check_vxlan_tunnel_state_table(dvs, src_ip, dip)

        tunnel_map_id = self.tunnel_map_map[vtep_name]

        decapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[2]
        encapstr = '2:' + tunnel_map_id[1] + ',' + tunnel_map_id[3]

        print(decapstr)
        print(encapstr)

        expected_tun_attributes = {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_PEER_MODE': 'SAI_TUNNEL_PEER_MODE_P2P',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                        'SAI_TUNNEL_ATTR_ENCAP_DST_IP': dip,
                        'SAI_TUNNEL_ATTR_ENCAP_TTL_MODE': 'SAI_TUNNEL_TTL_MODE_PIPE_MODEL',
                        'SAI_TUNNEL_ATTR_ENCAP_TTL_VAL': '255',
                    }
           
        ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_TABLE, expected_tun_attributes)
        assert len(ret) > 0, "Tunnel entry not created"
        assert len(ret) == 1, "More than 1 tunnel entry created"

        self.dip_tunnel_map[dip] = ret[0]
        tunnel_id = ret[0]

        expected_bridgeport_attributes = {
        'SAI_BRIDGE_PORT_ATTR_TYPE': 'SAI_BRIDGE_PORT_TYPE_TUNNEL',
        'SAI_BRIDGE_PORT_ATTR_TUNNEL_ID': tunnel_id,
        'SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE': 'SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE',
        'SAI_BRIDGE_PORT_ATTR_ADMIN_STATE': 'true',
        }

        ret = self.helper.get_key_with_attr(asic_db, self.ASIC_BRIDGE_PORT, expected_bridgeport_attributes)
        assert len(ret) > 0, "Bridgeport entry not created"
        assert len(ret) == 1, "More than 1 bridgeport entry created"
        
        self.bridgeport_map[dip] = ret[0]

    def check_vxlan_dip_tunnel_not_created(self, dvs, vtep_name, src_ip, dip):
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        expected_state_attributes = {
            'src_ip': src_ip,
            'dst_ip': dip,
            'tnl_src': 'EVPN',
        }

        ret = self.helper.get_key_with_attr(state_db, 'VXLAN_TUNNEL_TABLE', expected_state_attributes)
        assert len(ret) == 0, "Tunnel Statetable entry created"

    def check_vlan_extension_delete(self, dvs, vlan_name, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER')
        status, fvs = tbl.get(self.vlan_member_map[dip+vlan_name])
        assert status == False, "VLAN Member entry not deleted"

    def check_vlan_extension_delete_p2mp(self, dvs, vlan_name, sip, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER')
        status, fvs = tbl.get(self.l2mcgroup_member_map[dip+vlan_name])
        assert status == False, "L2MC Group Member entry not deleted"

    def check_vlan_obj(self, dvs, vlan_name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        expected_vlan_attributes = {
            'SAI_VLAN_ATTR_VLAN_ID': vlan_name,
        }
        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN', expected_vlan_attributes)
        assert len(ret) > 0, "VLAN entry not created"
        assert len(ret) == 1, "More than 1 VLAN entry created"

        self.vlan_id_map[vlan_name] = ret[0]

    def check_vlan_extension(self, dvs, vlan_name, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        expected_vlan_attributes = {
            'SAI_VLAN_ATTR_VLAN_ID': vlan_name,
        }
        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN', expected_vlan_attributes)
        assert len(ret) > 0, "VLAN entry not created"
        assert len(ret) == 1, "More than 1 VLAN entry created"

        self.vlan_id_map[vlan_name] = ret[0]

        expected_vlan_member_attributes = {
            'SAI_VLAN_MEMBER_ATTR_VLAN_ID': self.vlan_id_map[vlan_name],
            'SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID': self.bridgeport_map[dip],
        }
        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER', expected_vlan_member_attributes)
        assert len(ret) > 0, "VLAN Member not created"
        assert len(ret) == 1, "More than 1 VLAN member created"
        self.vlan_member_map[dip+vlan_name] = ret[0]

    def check_vlan_extension_not_created(self, dvs, vlan_name, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        expected_vlan_attributes = {
            'SAI_VLAN_ATTR_VLAN_ID': vlan_name,
        }
        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN', expected_vlan_attributes)
        assert len(ret) > 0, "VLAN entry not created"
        assert len(ret) == 1, "More than 1 VLAN entry created"

        self.vlan_id_map[vlan_name] = ret[0]

        if dip in self.bridgeport_map:
            expected_vlan_member_attributes = {
                    'SAI_VLAN_MEMBER_ATTR_VLAN_ID': self.vlan_id_map[vlan_name],
                    'SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID': self.bridgeport_map[dip],
                    }
            ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER', expected_vlan_member_attributes)
            assert len(ret) == 0, "VLAN member created"

    def check_vlan_extension_p2mp(self, dvs, vlan_name, sip, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN')
        expected_vlan_attributes = {
            'SAI_VLAN_ATTR_VLAN_ID': vlan_name,
        }
        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN', expected_vlan_attributes)
        assert len(ret) > 0, "VLAN entry not created"
        assert len(ret) == 1, "More than 1 VLAN entry created"

        self.vlan_id_map[vlan_name] = ret[0]
        status, fvs = tbl.get(self.vlan_id_map[vlan_name])

        print(fvs)

        uuc_flood_type = None
        bc_flood_type = None
        uuc_flood_group = None
        bc_flood_group = None

        for attr,value in fvs:
            if attr ==  'SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE':
                uuc_flood_type = value
            elif attr == 'SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE':
                bc_flood_type = value
            elif attr == 'SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_GROUP':
                uuc_flood_group = value
            elif attr == 'SAI_VLAN_ATTR_BROADCAST_FLOOD_GROUP':
                bc_flood_group = value

        assert uuc_flood_type == 'SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED', "Unknown unicast flood control type is not combined"
        assert bc_flood_type == 'SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED', "Broadcast flood control type is not combined"
        assert uuc_flood_group == bc_flood_group, "Unexpected two flood groups in VLAN"


        expected_l2mc_group_member_attributes = {
            'SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_GROUP_ID' : uuc_flood_group,
            'SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_ENDPOINT_IP': dip,
            'SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_OUTPUT_ID': self.bridgeport_map[sip],
        }

        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER', expected_l2mc_group_member_attributes)
        assert len(ret) > 0, "L2MC group Member not created"
        assert len(ret) == 1, "More than 1 L2MC group member created"
        self.l2mcgroup_member_map[dip+vlan_name] =  ret[0]
        
    def check_vlan_extension_not_created_p2mp(self, dvs, vlan_name, sip, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN')
        expected_vlan_attributes = {
            'SAI_VLAN_ATTR_VLAN_ID': vlan_name,
        }
        ret = self.helper.get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN', expected_vlan_attributes)
        assert len(ret) > 0, "VLAN entry not created"
        assert len(ret) == 1, "More than 1 VLAN entry created"

        self.vlan_id_map[vlan_name] = ret[0]
        status, fvs = tbl.get(self.vlan_id_map[vlan_name])

        print(fvs)

        uuc_flood_type = None
        bc_flood_type = None
        uuc_flood_group = None
        bc_flood_group = None

        for attr,value in fvs:
            assert attr != 'SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE', "Unknown unicast flood control type is set"
            assert attr != 'SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE', "Broadcast flood control type is set"
            assert attr != 'SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_GROUP', "Unknown unicast flood group is set"
            assert attr != 'SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE', "Broadcast flood group is set"

    def check_vxlan_tunnel_entry(self, dvs, tunnel_name, vnet_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        time.sleep(2)

        if (self.tunnel_map_map.get(tunnel_name) is None):
            tunnel_map_id = self.helper.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 2)
        else:
            tunnel_map_id = self.tunnel_map_map[tunnel_name]

        tunnel_map_entry_id = self.helper.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 2)

        # check that the vxlan tunnel termination are there
        assert self.helper.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 2), "The TUNNEL_MAP_ENTRY is created too early"

        self.helper.check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[1],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY': self.vr_map[vnet_name].get('ing'),
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE': vni_id,
            }
        )

        self.helper.check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[1],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[0],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE': self.vr_map[vnet_name].get('egr'),
            }
        )

        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)

    def check_vxlan_tunnel_vrf_map_entry(self, dvs, tunnel_name, vrf_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_map_entry_id = self.helper.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 2)

        # check that the vxlan tunnel termination are there
        assert self.helper.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 2), "The TUNNEL_MAP_ENTRY is created too early"

        ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY,
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY': self.vr_map[vrf_name].get('ing'),
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE': vni_id,
            }
        )

        assert len(ret) == 1, "Invalid number of tunnel map entries for SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI"

        self.tunnel_map_vrf_entry_ids.update(ret[0])

        ret = self.helper.get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY,
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE': self.vr_map[vrf_name].get('egr'),
            }
        )

        assert len(ret) == 1, "Invalid number of tunnel map entries for SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID"
        self.tunnel_map_vrf_entry_ids.update(ret[0])
        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)

    def check_vxlan_tunnel_vrf_map_entry_remove(self, dvs, tunnel_name, vrf_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_map_entry_id = self.helper.get_deleted_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 2)
        self.helper.check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0])
        self.helper.check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[1])
        for vrf_map_id in self.tunnel_map_vrf_entry_ids:
            self.helper.check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, vrf_map_id)

    def check_router_interface(self, dvs, name, vlan_oid, route_count):
        # Check RIF in ingress VRF
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        expected_attr = {
            "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID": self.vr_map[name].get('ing'),
            "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS": self.switch_mac,
        }

        if vlan_oid:
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_VLAN'})
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_VLAN_ID': vlan_oid})
        else:
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_PORT'})

        new_rif = self.helper.get_created_entry(asic_db, self.ASIC_RIF_TABLE, self.rifs)
        self.helper.check_object(asic_db, self.ASIC_RIF_TABLE, new_rif, expected_attr)

        #IP2ME route will be created with every router interface
        new_route = self.helper.get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, route_count)

        self.rifs.add(new_rif)
        self.routes.update(new_route)

    def check_del_router_interface(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        old_rif = self.helper.get_deleted_entries(asic_db, self.ASIC_RIF_TABLE, self.rifs, 1)
        self.helper.check_deleted_object(asic_db, self.ASIC_RIF_TABLE, old_rif[0])

        self.rifs.remove(old_rif[0])

    def vrf_route_ids(self, dvs, vrf_name):
        vr_set = set()

        vr_set.add(self.vr_map[vrf_name].get('ing'))
        return vr_set

    def check_vrf_routes(self, dvs, prefix, vrf_name, endpoint, tunnel, mac="", vni=0, no_update=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vrf_route_ids(dvs, vrf_name)
        count = len(vr_ids)

        # Check routes in ingress VRF
        expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                    }

        if vni:
            expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_VNI': vni})

        if mac:
            expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_MAC': mac})

        if endpoint in self.nh_ids:
            new_nh = self.nh_ids[endpoint]
        else:
            new_nh = self.helper.get_created_entry(asic_db, self.ASIC_NEXT_HOP, self.nhops)
            self.nh_ids[endpoint] = new_nh
            self.nhops.add(new_nh)

        self.helper.check_object(asic_db, self.ASIC_NEXT_HOP, new_nh, expected_attr)
        if no_update:
           count = 0
        new_route = self.helper.get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

        if count:
            self.route_id[vrf_name + ":" + prefix] = new_route

        #Check if the route is in expected VRF
        asic_vrs = set()
        for idx in range(count):
            self.helper.check_object(asic_db, self.ASIC_ROUTE_ENTRY, new_route[idx],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": new_nh,
                        }
                    )
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])
            found_route = False
            if rt_key['dest'] == prefix:
                found_route = True

            assert found_route

        if count:
            assert asic_vrs == vr_ids

            self.routes.update(new_route)

        return True

    def check_vrf_routes_absence(self, dvs, prefix, vrf_name, endpoint, tunnel, mac="", vni=0, no_update=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vrf_route_ids(dvs, vrf_name)
        count = len(vr_ids)

        # Check routes in ingress VRF
        expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                    }

        if vni:
            expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_VNI': vni})

        if mac:
            expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_MAC': mac})

        self.helper.get_created_entries(asic_db, self.ASIC_NEXT_HOP, self.nhops, 0)

    def check_vrf_routes_ecmp(self, dvs, prefix, vrf_name, tunnel, nh_count, no_update=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vrf_route_ids(dvs, vrf_name)
        count = len(vr_ids)

        new_nhg = self.helper.get_created_entry(asic_db, self.ASIC_NEXT_HOP_GRP, self.nhop_grp)
        self.nh_grp_id.add(new_nhg)

        if no_update:
           count = 0

        new_route = self.helper.get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)
        if count:
            self.route_id[vrf_name + ":" + prefix] = new_route

        #Check if the route is in expected VRF
        asic_vrs = set()
        for idx in range(count):
            self.helper.check_object(asic_db, self.ASIC_ROUTE_ENTRY, new_route[idx],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": new_nhg,
                        }
                    )
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])

            found_route = False
            if rt_key['dest'] == prefix:
                found_route = True

            assert found_route

        if count:
            assert asic_vrs == vr_ids

            self.routes.update(new_route)

        new_nhg_members = self.helper.get_created_entries(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS, self.nhop_grp_members, nh_count)

        for idx in range(nh_count):
            self.nh_grp_member_id.add(new_nhg_members[idx])
            self.helper.check_object(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS, new_nhg_members[idx],
                        {
                            "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID": new_nhg,
                        }
                    )

        nhid_list = list()

        nhg_member_tbl = swsscommon.Table(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS)

        for k in new_nhg_members:
            (status, fvs) = nhg_member_tbl.get(k)
            assert status

            for v in fvs:
                if v[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                    nhid = v[1]
                    nhid_list.append(nhid)

        return nhid_list

    def check_add_tunnel_nexthop(self, dvs, nh_id, endpoint, tunnel, mac, vni):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        # Check routes in ingress VRF
        expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                        "SAI_NEXT_HOP_ATTR_TUNNEL_VNI": vni,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_MAC": mac,
                    }

        self.helper.check_object(asic_db, self.ASIC_NEXT_HOP, nh_id, expected_attr)

    def check_del_tunnel_nexthop(self, dvs, vrf_name, endpoint, tunnel, mac="", vni=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        del_nh_ids = self.helper.get_deleted_entries(asic_db, self.ASIC_NEXT_HOP, self.nhops, 1)
        self.helper.check_deleted_object(asic_db, self.ASIC_NEXT_HOP, del_nh_ids[0])
        self.helper.check_deleted_object(asic_db, self.ASIC_NEXT_HOP, self.nh_ids[endpoint])
        assert del_nh_ids[0] == self.nh_ids[endpoint]
        self.nh_ids.pop(endpoint)
        return True

    def check_vrf_routes_ecmp_nexthop_grp_del(self, dvs, nh_count):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        nh_grp_id_len = len(self.nh_grp_id)
        assert nh_grp_id_len == 1

        for nh_grp_id in self.nh_grp_id:
            self.helper.check_deleted_object(asic_db, self.ASIC_NEXT_HOP_GRP, nh_grp_id)
        self.nh_grp_id.clear()

        nh_grp_member_id_len = len(self.nh_grp_member_id)
        assert nh_grp_member_id_len == nh_count

        for nh_grp_member_id in self.nh_grp_member_id:
            self.helper.check_deleted_object(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS, nh_grp_member_id)

        self.nh_grp_member_id.clear()

    def check_del_vrf_routes(self, dvs, prefix, vrf_name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        del_route = self.helper.get_deleted_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, 1)

        for idx in range(1):
            rt_key = json.loads(del_route[idx])
            found_route = False
            if rt_key['dest'] == prefix:
                found_route = True

            assert found_route

        self.helper.check_deleted_object(asic_db, self.ASIC_ROUTE_ENTRY, self.route_id[vrf_name + ":" + prefix])
        del self.route_id[vrf_name + ":" + prefix]

        return True

    def create_vrf(self, dvs, vrf_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        initial_entries = set(tbl.getKeys())

        attrs = [
            ("vni", "0"),
        ]
        tbl = swsscommon.Table(conf_db, "VRF")
        fvs = swsscommon.FieldValuePairs(attrs)
        tbl.set(vrf_name, fvs)
        time.sleep(2)

        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        current_entries = set(tbl.getKeys())
        assert len(current_entries - initial_entries) == 1

        new_vr_ids  = self.helper.get_created_entries(asic_db, self.ASIC_VRF_TABLE, self.vnet_vr_ids, 1)
        self.vnet_vr_ids.update(new_vr_ids)
        self.vr_map[vrf_name] = { 'ing':new_vr_ids[0], 'egr':new_vr_ids[0]}

        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, dvs, vrf_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(conf_db, "VRF")
        tbl._del(vrf_name)
        time.sleep(2)

    def is_vrf_attributes_correct(self, db, table, key, expected_attributes):
        tbl =  swsscommon.Table(db, table)
        keys = set(tbl.getKeys())
        assert key in keys, "The created key wasn't found"

        status, fvs = tbl.get(key)
        assert status, "Got an error when get a key"

        # filter the fake 'NULL' attribute out
        fvs = filter(lambda x : x != ('NULL', 'NULL'), fvs)

        attr_keys = {entry[0] for entry in fvs}
        assert attr_keys == set(expected_attributes.keys())

        for name, value in fvs:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                  (value, name, expected_attributes[name])

