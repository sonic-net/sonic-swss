from swsscommon import swsscommon
import json
from contextlib import ExitStack
import time
from dvslib.dvs_common import PollingConfig, wait_for_result
from evpn_tunnel import VxlanTunnel,VxlanEvpnHelper

def get_vxlan_p2p_tunnel_bp(db, remote_ip):
    tnl_id = None
    bp = None
    print("remote_ip = " + remote_ip)
    attributes = [("SAI_TUNNEL_ATTR_TYPE", "SAI_TUNNEL_TYPE_VXLAN"),
             ("SAI_TUNNEL_ATTR_ENCAP_DST_IP", remote_ip)
            ]
    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
    keys = tbl.getKeys()
    for key in keys:
        status, fvs = tbl.get(key)
        assert status, "Error reading from table ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
        attrs = dict(attributes)
        num_match = 0
        for k, v in fvs:
            print("attr:value="+str(k)+":"+str(v))
            if k in attrs and attrs[k] == v:
                num_match += 1
        if num_match == len(attributes):
            tnl_id = str(key)
            break
        else:
            tnl_id = None

    print("tnl_id = "+str(tnl_id))
    if tnl_id != None:
        tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        keys = tbl.getKeys()
        for key in keys:
            status, fvs = tbl.get(key)
            assert status, "Error reading from table ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"
            for k, v in fvs:
                print("attr:value="+str(k)+":"+str(v))
                if k == "SAI_BRIDGE_PORT_ATTR_TUNNEL_ID" and tnl_id == v:
                    bp = key
                    break
            if bp != None:
                break
    else:
        pass
    print("bp = "+str(bp))
    return bp


def get_asic_fdb_attrs(db, mac, bvid):
    table = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY")
    for key in table.getKeys():
        try:
            identity = json.loads(key)
        except (TypeError, ValueError):
            try:
                identity = json.loads("{" + key + "}")
            except (TypeError, ValueError):
                continue
        if identity.get("mac", "").lower() == mac.lower() and identity.get("bvid") == str(bvid):
            status, fields = table.get(key)
            assert status, "Unable to read ASIC FDB entry {}".format(key)
            return dict(fields)
    return {}


def get_asic_l2_nhg_bridge_port(db, old_nhg_ids):
    nhg_table = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
    bridge_port_table = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    for nhg_id in set(nhg_table.getKeys()) - old_nhg_ids:
        status, fields = nhg_table.get(nhg_id)
        if not status or dict(fields).get("SAI_NEXT_HOP_GROUP_ATTR_TYPE") != "SAI_NEXT_HOP_GROUP_TYPE_BRIDGE_PORT":
            continue
        for bridge_port_id in bridge_port_table.getKeys():
            bp_status, bp_fields = bridge_port_table.get(bridge_port_id)
            attrs = dict(bp_fields) if bp_status else {}
            if attrs.get("SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID") == nhg_id:
                return bridge_port_id
    return None


def test_evpn_fdb_ifname_vtep_nhg_moves(dvs, testlog):
    vxlan = VxlanTunnel()
    dvs.setup_db()
    dvs.clear_fdb()
    source_tunnel = "move_test_vtep"
    source_ip = "7.7.7.77"
    map_name = "move_test_map"
    remote_vtep_1 = "6.6.6.61"
    remote_vtep_2 = "8.8.8.82"
    mac = "52:54:00:25:06:f1"
    fdb_key = "Vlan3:" + mac

    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    fdb_producer = swsscommon.ProducerStateTable(app_db, "VXLAN_FDB_TABLE")
    fdb_table = swsscommon.Table(app_db, "VXLAN_FDB_TABLE")
    l2_nhg_producer = swsscommon.ProducerStateTable(app_db, "L2_NEXTHOP_GROUP_TABLE")
    nhg_asic = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
    old_nhg_ids = set(nhg_asic.getKeys())
    polling = PollingConfig(polling_interval=0.1, timeout=30, strict=True)

    def wait_for_fdb_destination(expected_bp, expected_endpoint=None):
        def ready():
            attrs = get_asic_fdb_attrs(dvs.adb, mac, dvs.getVlanOid("3"))
            endpoint = attrs.get("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP")
            endpoint_matches = (endpoint == expected_endpoint if expected_endpoint is not None
                                else endpoint in (None, "0.0.0.0", "::"))
            matches = (attrs.get("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID") == expected_bp and
                       attrs.get("SAI_FDB_ENTRY_ATTR_TYPE") == "SAI_FDB_ENTRY_TYPE_STATIC" and
                       attrs.get("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE") == "true" and
                       endpoint_matches)
            return matches, attrs
        return wait_for_result(ready, polling, "FDB destination did not converge")[1]

    def move_to(field, value, expected_bp, expected_endpoint=None):
        # ProducerStateTable merges fields. Drop the previous destination
        # fields without deleting the FDB row, so this remains a MAC move.
        for other_field in ("ifname", "remote_vtep", "nexthop_group"):
            if other_field != field:
                fdb_table.hdel(fdb_key, other_field)
        fdb_producer.set(fdb_key, swsscommon.FieldValuePairs([
            ("vni", "3"), ("type", "dynamic"), (field, value)
        ]))
        wait_for_fdb_destination(expected_bp, expected_endpoint)

    def remove_fdb():
        fdb_producer._del(fdb_key)
        wait_for_result(
            lambda: (not get_asic_fdb_attrs(dvs.adb, mac, dvs.getVlanOid("3")), None),
            polling, "FDB entry remained after test cleanup")

    def remove_group():
        l2_nhg_producer._del("200")
        wait_for_result(
            lambda: (get_asic_l2_nhg_bridge_port(dvs.adb, old_nhg_ids) is None and
                     not (set(nhg_asic.getKeys()) - old_nhg_ids), None),
            polling, "L2 next-hop-group remained after test cleanup")

    # ExitStack runs every cleanup even when setup or another cleanup fails.
    # Register before creation to cover partially completed setup operations.
    with ExitStack() as cleanup:
        cleanup.callback(dvs.remove_vlan, "3")
        dvs.create_vlan("3")
        cleanup.callback(dvs.remove_vlan_member, "3", "Ethernet0")
        dvs.create_vlan_member("3", "Ethernet0")
        cleanup.callback(vxlan.remove_vxlan_tunnel, dvs, source_tunnel)
        vxlan.create_vxlan_tunnel(dvs, source_tunnel, source_ip)
        cleanup.callback(vxlan.remove_evpn_nvo, dvs, "move_test_nvo")
        vxlan.create_evpn_nvo(dvs, "move_test_nvo", source_tunnel)
        cleanup.callback(vxlan.remove_vxlan_tunnel_map, dvs, source_tunnel, map_name, "3", "Vlan3")
        vxlan.create_vxlan_tunnel_map(dvs, source_tunnel, map_name, "3", "Vlan3")
        for remote_vtep in (remote_vtep_1, remote_vtep_2):
            cleanup.callback(vxlan.remove_evpn_remote_vni, dvs, "Vlan3", remote_vtep)
            vxlan.create_evpn_remote_vni(dvs, "Vlan3", remote_vtep, "3")

        def local_bridge_port_ready():
            bridge_port = dvs.get_map_iface_bridge_port_id(dvs.adb).get("Ethernet0")
            return bridge_port is not None, bridge_port

        def tunnel_bridge_port_ready():
            bridge_port = get_vxlan_p2p_tunnel_bp(dvs.adb, remote_vtep_1)
            return bridge_port is not None, bridge_port

        local_bp = wait_for_result(local_bridge_port_ready, polling)[1]
        vtep_bp = wait_for_result(tunnel_bridge_port_ready, polling)[1]
        assert local_bp != vtep_bp

        for nh_id, remote_vtep in (("100", remote_vtep_1), ("101", remote_vtep_2)):
            cleanup.callback(l2_nhg_producer._del, nh_id)
            l2_nhg_producer.set(nh_id, swsscommon.FieldValuePairs([
                ("remote_vtep", remote_vtep), ("source_vtep", source_tunnel)
            ]))
        cleanup.callback(remove_group)
        l2_nhg_producer.set("200", swsscommon.FieldValuePairs([
            ("nexthop_group", "100,101"), ("source_vtep", source_tunnel)
        ]))

        def nhg_bridge_port_ready():
            bridge_port = get_asic_l2_nhg_bridge_port(dvs.adb, old_nhg_ids)
            return bridge_port is not None, bridge_port
        nhg_bp = wait_for_result(nhg_bridge_port_ready, polling,
                                 "L2 next-hop-group bridge port was not created")[1]

        cleanup.callback(remove_fdb)
        move_to("ifname", "Ethernet0", local_bp)
        move_to("remote_vtep", remote_vtep_1, vtep_bp, remote_vtep_1)
        move_to("ifname", "Ethernet0", local_bp)
        move_to("nexthop_group", "200", nhg_bp)
        move_to("ifname", "Ethernet0", local_bp)


def test_evpnFdb(dvs, testlog):
    vxlan_obj = VxlanTunnel()
    helper = VxlanEvpnHelper()
    dvs.setup_db()

    dvs.clear_fdb()
    time.sleep(2)

    #Find switch_id
    switch_id = dvs.getSwitchOid()
    print("Switch_id="+str(switch_id))

    vlan_before = helper.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")

    # create vlan
    print("Creating Vlan3")
    dvs.create_vlan("3")
    time.sleep(2)

    vlan_after = helper.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    assert vlan_after - vlan_before == 1, "The Vlan3 wasn't created"
    print("Vlan3 is created")

    # Find the vlan_oid to be used in DB communications
    vlan_oid_3 = dvs.getVlanOid("3")
    assert vlan_oid_3 is not None, "Could not find Vlan_oid"
    print("Vlan-3 vlan_oid="+str(vlan_oid_3))

    bp_before = helper.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_before = helper.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    print("Making Ethernet0 as a member of Vlan3")
    dvs.create_vlan_member("3", "Ethernet0")
    time.sleep(2)

    # check that the vlan information was propagated
    bp_after = helper.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_after = helper.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    assert bp_after - bp_before == 1, "The bridge port wasn't created"
    assert vm_after - vm_before == 1, "The vlan member wasn't added"
    print("Ethernet0 is a member of Vlan3")

    # Get mapping between interface name and its bridge port_id
    iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

    #create SIP side of tunnel
    source_tnl_name = "source_vtep_name"
    source_tnl_ip = "7.7.7.7"
    vxlan_obj.create_vxlan_tunnel(dvs, source_tnl_name, source_tnl_ip)
    time.sleep(1)


    nvo_name = "evpn_nvo"
    vxlan_obj.create_evpn_nvo(dvs, nvo_name, source_tnl_name)
    time.sleep(1)


    map_name_vlan_3 = "map_3_3"
    vxlan_obj.create_vxlan_tunnel_map(dvs, source_tnl_name, map_name_vlan_3, "3", "Vlan3")
    time.sleep(1)


    remote_ip_6 = "6.6.6.6"
    vxlan_obj.create_evpn_remote_vni(dvs, "Vlan3", remote_ip_6, "3")
    remote_ip_8 = "8.8.8.8"
    vxlan_obj.create_evpn_remote_vni(dvs, "Vlan3", remote_ip_8, "3")
    time.sleep(1)

    #UT-1 Evpn Mac add from remote when tunnels are already created
    mac = "52:54:00:25:06:E9"
    print("Creating Evpn FDB Vlan3:"+mac.lower()+":6.6.6.6 in APP-DB")
    helper.create_entry_pst(
        dvs.pdb,
        "VXLAN_FDB_TABLE", "Vlan3:"+mac.lower(),
        [
            ("remote_vtep", remote_ip_6),
            ("type", "dynamic"),
            ("vni", "3")
        ]
    )
    time.sleep(1)

    tnl_bp_oid_6 = get_vxlan_p2p_tunnel_bp(dvs.adb, remote_ip_6)

    # check that the FDB entry is inserted into ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", vlan_oid_3)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true"),
                     ("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP", remote_ip_6),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", str(tnl_bp_oid_6)),
                    ]
    )
    assert ok == True, str(extra)
    print("EVPN FDB Vlan3:"+mac.lower()+":"+remote_ip_6+" is created in ASIC-DB")

    time.sleep(1)

    #UT-2 Evpn Mac del from remote
    mac = "52:54:00:25:06:E9"
    print("Deleting Evpn FDB Vlan3:"+mac.lower()+":6.6.6.6 in APP-DB")
    helper.delete_entry_pst(
        dvs.pdb,
        "VXLAN_FDB_TABLE", "Vlan3:"+mac.lower()
    )
    time.sleep(1)

    # check that the FDB entry is deleted from ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", vlan_oid_3)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true"),
                     ("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP", remote_ip_6),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", str(tnl_bp_oid_6)),
                    ]
    )
    assert ok == False, str(extra)
    print("EVPN FDB Vlan3:"+mac.lower()+":"+remote_ip_6+" is deleted from ASIC-DB")

    time.sleep(1)

    #UT-3 Evpn Mac add from remote when local mac is already present
    mac = "52:54:00:25:06:E9"

    print("Creating Local dynamic FDB Vlan3:"+mac.lower()+":Ethernet0 in APP-DB")
    # Create Dynamic MAC entry in APP DB
    helper.create_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan3:"+mac.lower(),
        [
            ("port", "Ethernet0"),
            ("type", "dynamic"),
        ]
    )

    time.sleep(1)
    #raw_input("Check ASIC_DB.........")

    # check that the FDB entry was added in ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", vlan_oid_3)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])]
    )
    assert ok, str(extra)
    print("Dynamic FDB Vlan3:"+mac.lower()+":Ethernet0 is created in Asic-DB")

    # check that the FDB entry was added in STATE DB
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
            "Vlan3:"+mac.lower(),
                    [("port", "Ethernet0"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found, str(extra)
    print("FDB Vlan3:"+mac+":Ethernet0 is created in STATE-DB")

    print("Creating Evpn FDB Vlan3:"+mac.lower()+":6.6.6.6 in APP-DB")
    helper.create_entry_pst(
        dvs.pdb,
        "VXLAN_FDB_TABLE", "Vlan3:"+mac.lower(),
        [
            ("remote_vtep", remote_ip_6),
            ("type", "dynamic"),
            ("vni", "3")
        ]
    )
    time.sleep(1)


    # check that the FDB entry is inserted into ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", str(dvs.getVlanOid("3")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true"),
                     ("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP", remote_ip_6),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", str(tnl_bp_oid_6)),
                    ]
    )
    assert ok, str(extra)
    print("EVPN FDB Vlan3:"+mac.lower()+":"+remote_ip_6+" is created in ASIC-DB")

    # check that the Local FDB entry is deleted from STATE DB
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
            "Vlan3:"+mac.lower(),
                    [("port", "Ethernet0"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("FDB Vlan3:"+mac+":Ethernet0 is deleted from STATE-DB")

    time.sleep(1)


    #UT-4 Evpn Sticky Mac add from remote
    mac = "52:54:00:25:06:E9"
    print("Creating Evpn Sticky FDB Vlan3:"+mac.lower()+":6.6.6.6 in APP-DB")
    helper.create_entry_pst(
        dvs.pdb,
        "VXLAN_FDB_TABLE", "Vlan3:"+mac.lower(),
        [
            ("remote_vtep", remote_ip_6),
            ("type", "static"),
            ("vni", "3")
        ]
    )
    time.sleep(1)


    # check that the FDB entry is inserted into ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", str(dvs.getVlanOid("3")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP", remote_ip_6),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", str(tnl_bp_oid_6)),
                    ]
    )
    assert ok, str(extra)
    print("EVPN Sticky FDB Vlan3:"+mac.lower()+":"+remote_ip_6+" is created in ASIC-DB")

    #raw_input("Check ASIC_DB.........")

    #UT-8 Evpn Mac add from remote when tunnels are already created
    mac = "52:54:00:25:06:E9"
    print("Creating Evpn FDB Vlan3:"+mac.lower()+":6.6.6.6 in APP-DB")
    helper.create_entry_pst(
        dvs.pdb,
        "VXLAN_FDB_TABLE", "Vlan3:"+mac.lower(),
        [
            ("remote_vtep", remote_ip_6),
            ("type", "dynamic"),
            ("vni", "3")
        ]
    )
    time.sleep(1)

    #raw_input("Check ASIC_DB.........")

    # check that the FDB entry is inserted into ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", vlan_oid_3)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true"),
                     ("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP", remote_ip_6),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", str(tnl_bp_oid_6)),
                    ]
    )
    assert ok == True, str(extra)
    print("EVPN FDB Vlan3:"+mac.lower()+":"+remote_ip_6+" is created in ASIC-DB")

    time.sleep(1)

    tnl_bp_oid_8 = get_vxlan_p2p_tunnel_bp(dvs.adb, remote_ip_8)

    print("Creating Evpn FDB Vlan3:"+mac.lower()+":8.8.8.8 in APP-DB")
    helper.create_entry_pst(
        dvs.pdb,
        "VXLAN_FDB_TABLE", "Vlan3:"+mac.lower(),
        [
            ("remote_vtep", remote_ip_8),
            ("type", "dynamic"),
            ("vni", "3")
        ]
    )
    time.sleep(1)

    #raw_input("Check ASIC_DB.........")

    # check that the FDB entry is inserted into ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", vlan_oid_3)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true"),
                     ("SAI_FDB_ENTRY_ATTR_ENDPOINT_IP", remote_ip_8),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", str(tnl_bp_oid_8)),
                    ]
    )
    assert ok == True, str(extra)
    print("EVPN FDB Vlan3:"+mac.lower()+":"+remote_ip_8+" is created in ASIC-DB")

    time.sleep(1)

    #UT-9 Local mac move (delete and learn) when remote is already added
    mac = "52:54:00:25:06:E9"
    print("Deleting FDB Vlan3:52-54-00-25-06-E9:8.8.8.8 in ASIC-DB")
    helper.delete_entry_tbl(dvs.adb, "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_3+"\",\"mac\":\""+mac+"\",\"switch_id\":\""+switch_id+"\"}")

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_3+"\\\",\\\"mac\\\":\\\""+mac+"\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_AGED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+str(tnl_bp_oid_8)+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    #raw_input("Check ASIC_DB.........")

    print("Creating FDB Vlan3:52-54-00-25-06-E9:Ethernet0 in ASIC-DB")
    helper.create_entry_tbl(
        dvs.adb,
        "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_3+"\",\"mac\":\"52:54:00:25:06:E9\",\"switch_id\":\""+switch_id+"\"}",
        [
            ("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
            ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"]),
        ]
    )

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_3+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_MOVE\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+iface_2_bridge_port_id["Ethernet0"]+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    #raw_input("Check ASIC_DB.........")

    # check that the FDB entry was added in ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "52:54:00:25:06:E9"), ("bvid", vlan_oid_3)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])]
    )
    assert ok, str(extra)
    print("FDB Vlan3:52-54-00-25-06-E9:Ethernet0 is created in ASIC-DB")

    # check that the FDB entry was added in STATE DB
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan3:52:54:00:25:06:e9",
                    [("port", "Ethernet0"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found, str(extra)
    print("FDB Vlan3:52-54-00-25-06-E9:Ethernet0 is created in STATE-DB")


    dvs.remove_vlan_member("3", "Ethernet0")
    dvs.remove_vlan("3")
