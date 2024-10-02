from swsscommon import swsscommon
import time
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
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_3+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_LEARNED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+iface_2_bridge_port_id["Ethernet0"]+"\"}]}]"
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
