import os
import re
import time
import json
import pytest

from swsscommon import swsscommon

def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def remove_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)
    
def verify_programmed_nh_membs(db,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size):
    nh_memb_count = {}
    for key in nh_memb_exp_count:
        nh_memb_count[key] = 0

    nhg_member_tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
    memb_dict = {}

    for tbs in nhg_member_tbl.getKeys():
        (status, fvs) = nhg_member_tbl.get(tbs)
        assert status == True
        index = -1
        nh_oid = "0"
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX":
                index = int(fv[1])
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                nh_oid = fv[1]
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                assert nhgid == fv[1]
        assert index != -1
        assert nh_oid != "0"
        assert nh_oid_map.get(nh_oid,"NULL") != "NULL"
        memb_dict[index] = nh_oid_map.get(nh_oid)
    idxs = [0]*bucket_size
    for idx,memb in memb_dict.items():
        nh_memb_count[memb] = 1 + nh_memb_count[memb]
        idxs[idx] = idxs[idx] + 1

    for key in nh_memb_exp_count:
        assert nh_memb_count[key] == nh_memb_exp_count[key]
    for idx in idxs:
        assert idx == 1

def shutdown_link(dvs, db, port):
    dvs.servers[port].runcmd("ip link set down dev eth0") == 0

    time.sleep(1)

    tbl = swsscommon.Table(db, "PORT_TABLE")
    (status, fvs) = tbl.get("Ethernet%d" % (port * 4))

    assert status == True

    oper_status = "unknown"

    for v in fvs:
        if v[0] == "oper_status":
            oper_status = v[1]
            break

    assert oper_status == "down"


def startup_link(dvs, db, port):
    dvs.servers[port].runcmd("ip link set up dev eth0") == 0

    time.sleep(1)

    tbl = swsscommon.Table(db, "PORT_TABLE")
    (status, fvs) = tbl.get("Ethernet%d" % (port * 4))

    assert status == True

    oper_status = "unknown"

    for v in fvs:
        if v[0] == "oper_status":
            oper_status = v[1]
            break

    assert oper_status == "up"


# Get state db route entry
def swss_get_route_entry_state(state_db,nh_memb_exp_count):
    stateroutetbl = swsscommon.Table(state_db, swsscommon.STATE_FG_ROUTE_TABLE_NAME)
    memb_dict = nh_memb_exp_count
    keys = stateroutetbl.getKeys()
    assert  len(keys) !=  0
    for key in keys:        
        (status, fvs) = stateroutetbl.get(key)
        assert status == True
        for fv in fvs:
            assert  fv[1] in nh_memb_exp_count
            memb_dict[fv[1]] = memb_dict[fv[1]] - 1 

    for idx,memb in memb_dict.items():
        assert memb == 0 


class TestFineGrainedNextHopGroup(object):
    def test_route_fgnhg(self, dvs, testlog):
        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        intf_tbl.set("Ethernet8", fvs)
        intf_tbl.set("Ethernet12", fvs)
        intf_tbl.set("Ethernet16", fvs)
        intf_tbl.set("Ethernet20", fvs)

        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        intf_tbl.set("Ethernet8|10.0.0.4/31", fvs)
        intf_tbl.set("Ethernet12|10.0.0.6/31", fvs)
        intf_tbl.set("Ethernet16|10.0.0.8/31", fvs)
        intf_tbl.set("Ethernet20|10.0.0.10/31", fvs)

        dvs.runcmd("config interface startup Ethernet0")
        dvs.runcmd("config interface startup Ethernet4")
        dvs.runcmd("config interface startup Ethernet8")
        dvs.runcmd("config interface startup Ethernet12")
        dvs.runcmd("config interface startup Ethernet16")
        dvs.runcmd("config interface startup Ethernet20")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0
        dvs.servers[1].runcmd("ip link set down dev eth0") == 0
        dvs.servers[2].runcmd("ip link set down dev eth0") == 0
        dvs.servers[3].runcmd("ip link set down dev eth0") == 0
        dvs.servers[4].runcmd("ip link set down dev eth0") == 0
        dvs.servers[5].runcmd("ip link set down dev eth0") == 0

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0
        dvs.servers[1].runcmd("ip link set up dev eth0") == 0
        dvs.servers[2].runcmd("ip link set up dev eth0") == 0
        dvs.servers[3].runcmd("ip link set up dev eth0") == 0
        dvs.servers[4].runcmd("ip link set up dev eth0") == 0
        dvs.servers[5].runcmd("ip link set up dev eth0") == 0
        
        fg_nhg_name = "fgnhg_v4"
        fg_nhg_prefix = "2.2.2.0/24"
        bucket_size = 60

        create_entry_tbl(
            config_db,
            "FG_NHG", '|', fg_nhg_name,
            [
                ("bucket_size", str(bucket_size)),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", '|', fg_nhg_prefix,
            [
                ("FG_NHG", fg_nhg_name),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.1",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.3",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )


        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.5",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.7",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.9",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.11",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet12,Ethernet16,Ethernet20")])

        ps.set(fg_nhg_prefix, fvs)

        time.sleep(1)

        # check if route was propagated to ASIC DB

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        rtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        nhgtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nhg_member_tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        nbtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        keys = rtbl.getKeys()

        found_route = False
        for k in keys:
            rt_key = json.loads(k)

            if rt_key['dest'] == fg_nhg_prefix:
                found_route = True
                break

        # Since we didn't populate ARP yet, the route shouldn't be programmed
        assert (found_route == False)

        dvs.runcmd("arp -s 10.0.0.1 00:00:00:00:00:01")
        dvs.runcmd("arp -s 10.0.0.3 00:00:00:00:00:02")
        dvs.runcmd("arp -s 10.0.0.5 00:00:00:00:00:03")
        dvs.runcmd("arp -s 10.0.0.9 00:00:00:00:00:05")
        dvs.runcmd("arp -s 10.0.0.11 00:00:00:00:00:06")
        time.sleep(1)

        keys = rtbl.getKeys()

        found_route = False
        for k in keys:
            rt_key = json.loads(k)

            if rt_key['dest'] == fg_nhg_prefix:
                found_route = True
                break
        
        # Now that ARP is populated, the route should be found
        assert found_route

        # assert the route points to next hop group
        (status, fvs) = rtbl.get(k)

        for v in fvs:
            if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                nhgid = v[1]

        (status, fvs) = nhgtbl.get(nhgid)
        assert status

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size

        # Obtain oids of NEXT_HOP asic entries
        nh_oid_map = {}

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        # Test addition of route with 0 members in bank
        # ARP is not resolved for 10.0.0.7, so fg nhg should be created with 10.0.0.7
        nh_memb_exp_count = {"10.0.0.9":30,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.9@Ethernet16":30,"10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        dvs.runcmd("arp -s 10.0.0.7 00:00:00:00:00:04")
        time.sleep(1)

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        # Now that ARP was resolved, 10.0.0.7 should be added as a valid fg nhg member
        nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Ethernet12":20, "10.0.0.9@Ethernet16":20,"10.0.0.11@Ethernet20":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring down 1 next hop
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.11"), ("ifname", "Ethernet12,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.7":30,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Ethernet12":30, "10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring up 1 next hop
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Ethernet12":20, "10.0.0.9@Ethernet16":20,"10.0.0.11@Ethernet20":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring up bank 0 next-hops in route for the 1st time
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":10,"10.0.0.3":10,"10.0.0.5":10,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":10, "10.0.0.3@Ethernet4":10,"10.0.0.5@Ethernet8":10, "10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring down arbitratry # of next-hops from both banks at the same time
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.11"), ("ifname", "Ethernet0,Ethernet8,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15, "10.0.0.5@Ethernet8":15, "10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring down 1 member and bring up 1 member in bank 0 at the same time
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.11"), ("ifname", "Ethernet0,Ethernet4,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.3":15,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15, "10.0.0.3@Ethernet4":15, "10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring down 2 members and bring up 1 member in bank 0 at the same time
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.5,10.0.0.11"), ("ifname", "Ethernet8,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.5@Ethernet8":30, "10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring up 2 members and bring down 1 member in bank 0 at the same time
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.11"), ("ifname", "Ethernet0,Ethernet4,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.3":15,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15,"10.0.0.3@Ethernet4":15,"10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bringup arbitrary # of next-hops from both banks at the same time
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":10,"10.0.0.3":10,"10.0.0.5":10,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":10, "10.0.0.3@Ethernet4":10,"10.0.0.5@Ethernet8":10, "10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Test bank down
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"), ("ifname", "Ethernet0,Ethernet4,Ethernet8")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":20,"10.0.0.3":20,"10.0.0.5":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":20, "10.0.0.3@Ethernet4":20,"10.0.0.5@Ethernet8":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Test bank down: nh change in active bank
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5"), ("ifname", "Ethernet0,Ethernet8")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":30,"10.0.0.5":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":30, "10.0.0.5@Ethernet8":30}
        swss_get_route_entry_state(state_db, nh__exp_count)


        # Test 1st memb up in bank
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.11"), ("ifname", "Ethernet0,Ethernet8,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.11":30}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15, "10.0.0.5@Ethernet8":15, "10.0.0.11@Ethernet20":30}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Test 2nd,3rd memb up in bank
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet0,Ethernet8,Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)

        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15,"10.0.0.5@Ethernet8":15, "10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # bring all links down one by one
        shutdown_link(dvs, db, 0)	
        nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.5@Ethernet8":30, "10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        shutdown_link(dvs, db, 2)
        nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Ethernet12":20, "10.0.0.9@Ethernet16":20,"10.0.0.11@Ethernet20":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        shutdown_link(dvs, db, 3)
        shutdown_link(dvs, db, 4)
        nh_memb_exp_count = {"10.0.0.11":60}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.11@Ethernet20":60}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Bring down last link, there shouldn't be a crash or other bad orchagent state because of this
        shutdown_link(dvs, db, 5)

	# bring all links up one by one
        startup_link(dvs, db, 3)
        startup_link(dvs, db, 4)
        startup_link(dvs, db, 5)
        nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Ethernet12":20, "10.0.0.9@Ethernet16":20,"10.0.0.11@Ethernet20":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        startup_link(dvs, db, 2)
        nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.5@Ethernet8":30,"10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        startup_link(dvs, db, 0)
        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15,"10.0.0.5@Ethernet8":15,"10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # remove fgnhg member
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.1",
        )
        nh_memb_exp_count = {"10.0.0.5":30,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.5@Ethernet8":30,"10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # add fgnhg member
        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.1",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )
        nh_memb_exp_count = {"10.0.0.1":15,"10.0.0.5":15,"10.0.0.7":10,"10.0.0.9":10,"10.0.0.11":10}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.1@Ethernet0":15,"10.0.0.5@Ethernet8":15,"10.0.0.7@Ethernet12":10, "10.0.0.9@Ethernet16":10,"10.0.0.11@Ethernet20":10}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # Remove route
        ps._del(fg_nhg_prefix)
        time.sleep(1)

        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)

            assert rt_key['dest'] != fg_nhg_prefix

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 0
        
        stateroute_tbl = swsscommon.Table(state_db, swsscommon.STATE_FG_ROUTE_TABLE_NAME)
        keys = stateroute_tbl.getKeys()
        assert len(keys) == 0
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", 
            fg_nhg_prefix,
        )

        # add normal route
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)

        time.sleep(1)
        
        # add fgnhg prefix
        create_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", '|', fg_nhg_prefix,
            [
                ("FG_NHG", fg_nhg_name),
            ],
        )
        
        keys = rtbl.getKeys()

        found_route = False
        for k in keys:
            rt_key = json.loads(k)

            if rt_key['dest'] == fg_nhg_prefix:
                found_route = True
                break

        assert found_route
        # assert the route points to next hop group
        (status, fvs) = rtbl.get(k)

        for v in fvs:
            if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                nhgid = v[1]

        (status, fvs) = nhgtbl.get(nhgid)
        assert status

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size

        # Obtain oids of NEXT_HOP asic entries
        nh_oid_map = {}

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        nh_memb_exp_count = {"10.0.0.7":20,"10.0.0.9":20,"10.0.0.11":20}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)
        nh__exp_count = {"10.0.0.7@Ethernet12":20, "10.0.0.9@Ethernet16":20,"10.0.0.11@Ethernet20":20}
        swss_get_route_entry_state(state_db, nh__exp_count)

        # remove fgnhg prefix
        remove_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", 
            fg_nhg_prefix,
        )

        time.sleep(1)

        # check ASIC route database
        for key in rtbl.getKeys():
            route = json.loads(key)
            if route["dest"] == fg_nhg_prefix:
                route_found = True
        assert route_found == True
        
        # remove prefix entry
        ps._del(fg_nhg_prefix)
        time.sleep(1)

        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)

            assert rt_key['dest'] != fg_nhg_prefix

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 0

        # remove group fail since there's still nexthop member
        remove_entry_tbl(
            config_db,
            "FG_NHG", 
            fg_nhg_name,
        )
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.1",
        )
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.3",
        )
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.5",
        )
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.7",
        )
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.9",
        )
        
        remove_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", 
            "10.0.0.11",
        )
       
        # remove group should succeeds
        remove_entry_tbl(
            config_db,
            "FG_NHG", 
            fg_nhg_name,
        )

        intf_tbl.set("Ethernet24", fvs)
        intf_tbl.set("Ethernet28", fvs)
        intf_tbl.set("Ethernet32", fvs)
        intf_tbl.set("Ethernet36", fvs)

        intf_tbl.set("Ethernet24|10.0.0.12/31", fvs)
        intf_tbl.set("Ethernet28|10.0.0.14/31", fvs)
        intf_tbl.set("Ethernet32|10.0.0.16/31", fvs)
        intf_tbl.set("Ethernet36|10.0.0.18/31", fvs)

        dvs.runcmd("config interface startup Ethernet24")
        dvs.runcmd("config interface startup Ethernet28")
        dvs.runcmd("config interface startup Ethernet32")
        dvs.runcmd("config interface startup Ethernet36")

        dvs.servers[6].runcmd("ip link set down dev eth0") == 0
        dvs.servers[7].runcmd("ip link set down dev eth0") == 0
        dvs.servers[8].runcmd("ip link set down dev eth0") == 0
        dvs.servers[9].runcmd("ip link set down dev eth0") == 0

        dvs.servers[6].runcmd("ip link set up dev eth0") == 0
        dvs.servers[7].runcmd("ip link set up dev eth0") == 0
        dvs.servers[8].runcmd("ip link set up dev eth0") == 0
        dvs.servers[9].runcmd("ip link set up dev eth0") == 0

        fg_nhg_name = "new_fgnhg_v4"
        fg_nhg_prefix = "3.3.3.0/24"
        # Test with non-divisible bucket size
        bucket_size = 128

        create_entry_tbl(
            config_db,
            "FG_NHG", '|', fg_nhg_name,
            [
                ("bucket_size", str(bucket_size)),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", '|', fg_nhg_prefix,
            [
                ("FG_NHG", fg_nhg_name),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.1",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.3",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )


        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.5",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.7",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.9",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "1"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.11",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.13",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.15",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.17",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )

        create_entry_tbl(
            config_db,
            "FG_NHG_MEMBER", '|', "10.0.0.19",
            [
                ("FG_NHG", fg_nhg_name),
                ("bank", "0"),
            ],
        )
        dvs.runcmd("arp -s 10.0.0.13 00:00:00:00:00:13")
        dvs.runcmd("arp -s 10.0.0.15 00:00:00:00:00:15")
        dvs.runcmd("arp -s 10.0.0.17 00:00:00:00:00:17")
        dvs.runcmd("arp -s 10.0.0.19 00:00:00:00:00:19")

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.11"), 
            ("ifname", "Ethernet0,Ethernet20")])

        ps.set(fg_nhg_prefix, fvs)

        time.sleep(1)

        # check if route was propagated to ASIC DB

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        rtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        nhgtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nhg_member_tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        nbtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        keys = rtbl.getKeys()

        found_route = False
        for k in keys:
            rt_key = json.loads(k)

            if rt_key['dest'] == fg_nhg_prefix:
                found_route = True
                break

        assert found_route
        # assert the route points to next hop group
        (status, fvs) = rtbl.get(k)

        for v in fvs:
            if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                nhgid = v[1]

        (status, fvs) = nhgtbl.get(nhgid)
        assert status

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == bucket_size

        # Obtain oids of NEXT_HOP asic entries
        nh_oid_map = {}

        for tbs in nbtbl.getKeys():
            (status, fvs) = nbtbl.get(tbs)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    nh_oid_map[tbs] = fv[1]

        # Test addition of route with 0 members in bank
        nh_memb_exp_count = {"10.0.0.1":64,"10.0.0.11":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)

        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.11,10.0.0.13,10.0.0.15"), 
            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet20,Ethernet24,Ethernet28")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":22,"10.0.0.3":21,"10.0.0.5":21,"10.0.0.11":22,"10.0.0.13":21,"10.0.0.15":21}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11,10.0.0.13,10.0.0.15,10.0.0.17,10.0.0.19"), 
            ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20,Ethernet24,Ethernet28,Ethernet32,Ethernet36")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":13,"10.0.0.3":13,"10.0.0.5":13,"10.0.0.7":12,"10.0.0.9":13,"10.0.0.11":13,"10.0.0.13":13,"10.0.0.15":13,"10.0.0.17":12,"10.0.0.19":13}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11,10.0.0.13,10.0.0.19"), 
            ("ifname", "Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20,Ethernet24,Ethernet36")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.3":16,"10.0.0.5":16,"10.0.0.7":16,"10.0.0.9":16,"10.0.0.11":22,"10.0.0.13":21,"10.0.0.19":21}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.3,10.0.0.7,10.0.0.9,10.0.0.13,10.0.0.15,10.0.0.17,10.0.0.19"), 
            ("ifname", "Ethernet4,Ethernet12,Ethernet16,Ethernet24,Ethernet28,Ethernet32,Ethernet36")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.3":22,"10.0.0.7":21,"10.0.0.9":21,"10.0.0.13":16,"10.0.0.15":16,"10.0.0.17":16,"10.0.0.19":16}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.7,10.0.0.11"), ("ifname", "Ethernet12,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.7":64,"10.0.0.11":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.5,10.0.0.7,10.0.0.9"), ("ifname", "Ethernet8,Ethernet12,Ethernet16")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.5":42,"10.0.0.7":44,"10.0.0.9":42}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5,10.0.0.7,10.0.0.9,10.0.0.11"), ("ifname", "Ethernet0,Ethernet4,Ethernet8,Ethernet12,Ethernet16,Ethernet20")])
        ps.set(fg_nhg_prefix, fvs)
        time.sleep(1)
        nh_memb_exp_count = {"10.0.0.1":12,"10.0.0.3":13,"10.0.0.5":13,"10.0.0.7":13,"10.0.0.9":13,"10.0.0.11":64}
        verify_programmed_nh_membs(adb,nh_memb_exp_count,nh_oid_map,nhgid,bucket_size)


        # Remove route
        ps._del(fg_nhg_prefix)
        time.sleep(1)

        keys = rtbl.getKeys()
        for k in keys:
            rt_key = json.loads(k)

            assert rt_key['dest'] != fg_nhg_prefix

        keys = nhg_member_tbl.getKeys()
        assert len(keys) == 0


        # remove fgnhg prefix
        remove_entry_tbl(
            config_db,
            "FG_NHG_PREFIX", 
            fg_nhg_prefix,
        )

