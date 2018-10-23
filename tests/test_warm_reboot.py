from swsscommon import swsscommon
import os
import re
import time
import json

# Get restore count of all processes supporting warm restart
def swss_get_RestoreCount(state_db):
    restore_count = {}
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    assert  len(keys) !=  0
    for key in keys:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restore_count":
                restore_count[key] = int(fv[1])
    print(restore_count)
    return restore_count

# function to check the restore count incremented by 1 for all processes supporting warm restart
def swss_check_RestoreCount(state_db, restore_count):
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    print(keys)
    assert  len(keys) > 0
    for key in keys:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restore_count":
                assert int(fv[1]) == restore_count[key] + 1
            elif fv[0] == "state":
                assert fv[1] == "reconciled"

def check_port_oper_status(appl_db, port_name, state):
    portTbl = swsscommon.Table(appl_db, swsscommon.APP_PORT_TABLE_NAME)
    (status, fvs) = portTbl.get(port_name)
    assert status == True

    oper_status = "unknown"
    for v in fvs:
        if v[0] == "oper_status":
            oper_status = v[1]
            break
    assert oper_status == state

# function to check the restore count incremented by 1 for a single process
def swss_app_check_RestoreCount_single(state_db, restore_count, name):
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    print(keys)
    print(restore_count)
    assert  len(keys) > 0
    for key in keys:
        if key != name:
            continue
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restore_count":
                assert int(fv[1]) == restore_count[key] + 1
            elif fv[0] == "state":
                assert fv[1] == "reconciled"

def swss_app_check_warmstart_state(state_db, name, state):
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    print(keys)
    assert  len(keys) > 0
    for key in keys:
        if key != name:
            continue
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "state":
                assert fv[1] == state

def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

    # FIXME: better to wait until DB create them
    time.sleep(1)

def create_entry_tbl(db, table, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def del_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)

def create_entry_pst(db, table, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def test_PortSyncdWarmRestart(dvs, testlog):

    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    dvs.runcmd("config warm_restart enable swss")

    dvs.runcmd("ifconfig Ethernet16  up")
    dvs.runcmd("ifconfig Ethernet20  up")

    time.sleep(1)

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet16|11.0.0.1/29", fvs)
    intf_tbl.set("Ethernet20|11.0.0.9/29", fvs)
    dvs.runcmd("ifconfig Ethernet16 up")
    dvs.runcmd("ifconfig Ethernet20 up")

    dvs.servers[4].runcmd("ip link set down dev eth0") == 0
    dvs.servers[4].runcmd("ip link set up dev eth0") == 0
    dvs.servers[4].runcmd("ifconfig eth0 11.0.0.2/29")
    dvs.servers[4].runcmd("ip route add default via 11.0.0.1")

    dvs.servers[5].runcmd("ip link set down dev eth0") == 0
    dvs.servers[5].runcmd("ip link set up dev eth0") == 0
    dvs.servers[5].runcmd("ifconfig eth0 11.0.0.10/29")
    dvs.servers[5].runcmd("ip route add default via 11.0.0.9")

    time.sleep(1)

    # Ethernet port oper status should be up
    check_port_oper_status(appl_db, "Ethernet16", "up")
    check_port_oper_status(appl_db, "Ethernet20", "up")

    # Ping should work between servers via vs vlan interfaces
    ping_stats = dvs.servers[4].runcmd("ping -c 1 11.0.0.10")
    time.sleep(1)

    neighTbl = swsscommon.Table(appl_db, "NEIGH_TABLE")
    (status, fvs) = neighTbl.get("Ethernet16:11.0.0.2")
    assert status == True

    (status, fvs) = neighTbl.get("Ethernet20:11.0.0.10")
    assert status == True

    restore_count = swss_get_RestoreCount(state_db)

    # restart portsyncd
    dvs.runcmd(['sh', '-c', 'pkill -x portsyncd'])

    pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE")
    dvs.runcmd(['sh', '-c', 'supervisorctl start portsyncd'])

    (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
    assert nadd == 0
    assert ndel == 0

    #new ip on server 5
    dvs.servers[5].runcmd("ifconfig eth0 11.0.0.11/29")

    # Ping should work between servers via vs Ethernet interfaces
    ping_stats = dvs.servers[4].runcmd("ping -c 1 11.0.0.11")

    # new neighbor learn on VS
    (status, fvs) = neighTbl.get("Ethernet20:11.0.0.11")
    assert status == True

    # Port state change reflected in appDB correctly
    dvs.servers[6].runcmd("ip link set down dev eth0") == 0
    dvs.servers[6].runcmd("ip link set up dev eth0") == 0
    time.sleep(1)

    check_port_oper_status(appl_db, "Ethernet16", "up")
    check_port_oper_status(appl_db, "Ethernet20", "up")
    check_port_oper_status(appl_db, "Ethernet24", "up")


    swss_app_check_RestoreCount_single(state_db, restore_count, "portsyncd")


def test_VlanMgrdWarmRestart(dvs, testlog):

    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    dvs.runcmd("ifconfig Ethernet16  0")
    dvs.runcmd("ifconfig Ethernet20  0")

    dvs.runcmd("ifconfig Ethernet16  up")
    dvs.runcmd("ifconfig Ethernet20  up")

    time.sleep(1)

    dvs.runcmd("config warm_restart enable swss")

    # create vlan
    create_entry_tbl(
        conf_db,
        "VLAN", "Vlan16",
        [
            ("vlanid", "16"),
        ]
    )
    # create vlan
    create_entry_tbl(
        conf_db,
        "VLAN", "Vlan20",
        [
            ("vlanid", "20"),
        ]
    )
    # create vlan member entry in config db. Don't use Ethernet0/4/8/12 as IP configured on them in previous testing.
    create_entry_tbl(
        conf_db,
        "VLAN_MEMBER", "Vlan16|Ethernet16",
         [
            ("tagging_mode", "untagged"),
         ]
    )

    create_entry_tbl(
        conf_db,
        "VLAN_MEMBER", "Vlan20|Ethernet20",
         [
            ("tagging_mode", "untagged"),
         ]
    )

    time.sleep(1)

    dvs.runcmd("ifconfig Vlan16 11.0.0.1/29 up")
    dvs.runcmd("ifconfig Vlan20 11.0.0.9/29 up")

    dvs.servers[4].runcmd("ifconfig eth0 11.0.0.2/29")
    dvs.servers[4].runcmd("ip route add default via 11.0.0.1")

    dvs.servers[5].runcmd("ifconfig eth0 11.0.0.10/29")
    dvs.servers[5].runcmd("ip route add default via 11.0.0.9")

    time.sleep(1)

    # Ping should work between servers via vs vlan interfaces
    ping_stats = dvs.servers[4].runcmd("ping -c 1 11.0.0.10")
    time.sleep(1)

    tbl = swsscommon.Table(appl_db, "NEIGH_TABLE")
    (status, fvs) = tbl.get("Vlan16:11.0.0.2")
    assert status == True

    (status, fvs) = tbl.get("Vlan20:11.0.0.10")
    assert status == True

    (exitcode, bv_before) = dvs.runcmd("bridge vlan")
    print(bv_before)

    restore_count = swss_get_RestoreCount(state_db)

    dvs.runcmd(['sh', '-c', 'pkill -x vlanmgrd'])

    pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE")

    dvs.runcmd(['sh', '-c', 'supervisorctl start vlanmgrd'])
    time.sleep(2)

    (exitcode, bv_after) = dvs.runcmd("bridge vlan")
    assert bv_after == bv_before

    (nadd, ndel) = dvs.CountSubscribedObjects(pubsub, ignore=["SAI_OBJECT_TYPE_FDB_ENTRY"])
    assert nadd == 0
    assert ndel == 0

    #new ip on server 5
    dvs.servers[5].runcmd("ifconfig eth0 11.0.0.11/29")

    # Ping should work between servers via vs vlan interfaces
    ping_stats = dvs.servers[4].runcmd("ping -c 1 11.0.0.11")

    # new neighbor learn on VS
    (status, fvs) = tbl.get("Vlan20:11.0.0.11")
    assert status == True

    swss_app_check_RestoreCount_single(state_db, restore_count, "vlanmgrd")

def stop_neighsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'pkill -x neighsyncd'])

def start_neighsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start neighsyncd'])

def check_no_neighsyncd_timer(dvs):
    (exitcode, string) = dvs.runcmd(['sh', '-c', 'grep getWarmStartTimer /var/log/syslog | grep neighsyncd | grep invalid'])
    assert string.strip() != ""

def check_neighsyncd_timer(dvs, timer_value):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "grep getWarmStartTimer /var/log/syslog | grep neighsyncd | tail -n 1 | rev | cut -d ' ' -f 1 | rev"])
    assert num.strip() == timer_value

# function to check neighbor entry reconciliation status written in syslog
def check_syslog_for_neighbor_entry(dvs, marker, new_cnt, delete_cnt, iptype):
    # check reconciliation results (new or delete entries) for ipv4 and ipv6
    if iptype == "ipv4" or iptype == "ipv6":
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep neighsyncd | grep cache-state:NEW | grep -i %s | wc -l" % (marker, iptype)])
        assert num.strip() == str(new_cnt)
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep neighsyncd | grep -E \"cache-state:(DELETE|STALE)\" | grep -i %s | wc -l" % (marker, iptype)])
        assert num.strip() == str(delete_cnt)
    else:
        assert "iptype is unknown" == ""

def test_swss_neighbor_syncup(dvs, testlog):

    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    dvs.runcmd("config warm_restart enable swss")

    #
    # Testcase1:
    # Add neighbor entries in linux kernel, appDB should get all of them
    #

    # create neighbor entries (4 ipv4 and 4 ip6, two each on each interface) in linux kernel
    intfs = ["Ethernet24", "Ethernet28"]
    #enable ipv6 on docker
    dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet24|24.0.0.1/24", fvs)
    intf_tbl.set("Ethernet28|28.0.0.9/24", fvs)
    intf_tbl.set("Ethernet24|2400::1/64", fvs)
    intf_tbl.set("Ethernet28|2800::1/64", fvs)
    dvs.runcmd("ifconfig Ethernet24 up")
    dvs.runcmd("ifconfig Ethernet28 up")

    ips = ["24.0.0.2", "24.0.0.3", "28.0.0.2", "28.0.0.3"]
    v6ips = ["2400::2", "2400::3", "2800::2", "2800::3"]

    macs = ["00:00:00:00:24:02", "00:00:00:00:24:03", "00:00:00:00:28:02", "00:00:00:00:28:03"]

    for i in range(len(ips)):
        dvs.runcmd("ip neigh add {} dev {} lladdr {}".format(ips[i], intfs[i%2], macs[i]))

    for i in range(len(v6ips)):
        dvs.runcmd("ip -6 neigh add {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], macs[i]))

    time.sleep(1)

    # Check the neighbor entries are inserted correctly
    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "NEIGH_TABLE")

    for i in range(len(ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], ips[i]))
        assert status == True

        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv4"

    for i in range(len(v6ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], v6ips[i]))
        assert status == True

        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv6"

    #
    # Testcase 2:
    # Restart neighsyncd without change neighbor entries, nothing should be sent to appDB or sairedis,
    # appDB should be kept the same.
    #

    # get restore_count
    restore_count = swss_get_RestoreCount(state_db)

    # stop neighsyncd and sairedis.rec
    stop_neighsyncd(dvs)
    marker = dvs.add_log_marker()
    pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
    start_neighsyncd(dvs)
    time.sleep(10)

    # Check the neighbor entries are still in appDB correctly
    for i in range(len(ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], ips[i]))
        assert status == True

        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv4"

    for i in range(len(v6ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], v6ips[i]))
        assert status == True

        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv6"

    # check syslog and sairedis.rec file for activities
    check_syslog_for_neighbor_entry(dvs, marker, 0, 0, "ipv4")
    check_syslog_for_neighbor_entry(dvs, marker, 0, 0, "ipv6")
    (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
    assert nadd == 0
    assert ndel == 0

    # check restore Count
    swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")

    #
    # Testcase 3:
    # stop neighsyncd, delete even nummber ipv4/ipv6 neighbor entries from each interface, warm start neighsyncd.
    # the neighsyncd is supposed to sync up the entries from kernel after warm restart
    # note: there was an issue for neighbor delete, it will be marked as FAILED instead of deleted in kernel
    #       but it will send netlink message to be removed from appDB, so it works ok here,
    #       just that if we want to add the same neighbor again, use "change" instead of "add"

    # get restore_count
    restore_count = swss_get_RestoreCount(state_db)

    # stop neighsyncd
    stop_neighsyncd(dvs)
    marker = dvs.add_log_marker()

    # delete even nummber of ipv4/ipv6 neighbor entries from each interface
    for i in range(0, len(ips), 2):
        dvs.runcmd("ip neigh del {} dev {}".format(ips[i], intfs[i%2]))

    for i in range(0, len(v6ips), 2):
        dvs.runcmd("ip -6 neigh del {} dev {}".format(v6ips[i], intfs[i%2]))

    # start neighsyncd again
    start_neighsyncd(dvs)
    time.sleep(10)

    # check ipv4 and ipv6 neighbors
    for i in range(len(ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], ips[i]))
        #should not see deleted neighbor entries
        if i %2 == 0:
            assert status == False
            continue
        else:
            assert status == True

        #undeleted entries should still be there.
        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv4"

    for i in range(len(v6ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], v6ips[i]))
        #should not see deleted neighbor entries
        if i %2 == 0:
            assert status == False
            continue
        else:
            assert status == True

        #undeleted entries should still be there.
        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv6"

    # check syslog and sairedis.rec file for activities
    # 2 deletes each for ipv4 and ipv6
    # 4 neighbor removal in asic db
    check_syslog_for_neighbor_entry(dvs, marker, 0, 2, "ipv4")
    check_syslog_for_neighbor_entry(dvs, marker, 0, 2, "ipv6")
    (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
    assert nadd == 0
    assert ndel == 4

    # check restore Count
    swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")


    #
    # Testcase 4:
    # Stop neighsyncd, add even nummber of ipv4/ipv6 neighbor entries to each interface again, 
    # Start neighsyncd
    # The neighsyncd is supposed to sync up the entries from kernel after warm restart
    # Check the timer is not retrieved from configDB since it is not configured

    # get restore_count
    restore_count = swss_get_RestoreCount(state_db)

    # stop neighsyncd
    stop_neighsyncd(dvs)
    marker = dvs.add_log_marker()

    # add even nummber of ipv4/ipv6 neighbor entries to each interface
    # use "change" if neighbor is in FAILED state
    for i in range(0, len(ips), 2):
        (rc, output) = dvs.runcmd(['sh', '-c', "ip -4 neigh | grep {}".format(ips[i])])
        print output
        if rc == 0:
            dvs.runcmd("ip neigh change {} dev {} lladdr {}".format(ips[i], intfs[i%2], macs[i]))
        else:
            dvs.runcmd("ip neigh add {} dev {} lladdr {}".format(ips[i], intfs[i%2], macs[i]))

    for i in range(0, len(v6ips), 2):
        (rc, output) = dvs.runcmd(['sh', '-c', "ip -6 neigh | grep {}".format(v6ips[i])])
        print output
        if rc == 0:
            dvs.runcmd("ip -6 neigh change {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], macs[i]))
        else:
            dvs.runcmd("ip -6 neigh add {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], macs[i]))

    # start neighsyncd again
    start_neighsyncd(dvs)
    time.sleep(10)

    # no neighsyncd timer configured
    check_no_neighsyncd_timer(dvs)

    # check ipv4 and ipv6 neighbors, should see all neighbors
    for i in range(len(ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], ips[i]))
        assert status == True
        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv4"

    for i in range(len(v6ips)):
        (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], v6ips[i]))
        assert status == True
        for v in fvs:
            if v[0] == "neigh":
                assert v[1] == macs[i]
            if v[0] == "family":
                assert v[1] == "IPv6"

    # check syslog and asic db for activities
    # 2 news entries for ipv4 and ipv6 each
    # 4 neighbor creation in asic db
    check_syslog_for_neighbor_entry(dvs, marker, 2, 0, "ipv4")
    check_syslog_for_neighbor_entry(dvs, marker, 2, 0, "ipv6")
    (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
    assert nadd == 4
    assert ndel == 0

    # check restore Count
    swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")

    #
    # Testcase 5:
    # Even number of ip4/6 neigbors updated with new mac.
    # Odd number of ipv4/6 neighbors removed and added to different interfaces.
    # neighbor syncd should sync it up after warm restart
    # include the timer settings in this testcase

    # setup timer in configDB
    timer_value = "15"

    dvs.runcmd("config warm_restart neighsyncd_timer {}".format(timer_value))

    # get restore_count
    restore_count = swss_get_RestoreCount(state_db)

    # stop neighsyncd
    stop_neighsyncd(dvs)
    marker = dvs.add_log_marker()

    # Even number of ip4/6 neigbors updated with new mac.
    # Odd number of ipv4/6 neighbors removed and added to different interfaces.
    newmacs = ["00:00:00:01:12:02", "00:00:00:01:12:03", "00:00:00:01:16:02", "00:00:00:01:16:03"]

    for i in range(len(ips)):
        if i % 2 == 0:
            dvs.runcmd("ip neigh change {} dev {} lladdr {}".format(ips[i], intfs[i%2], newmacs[i]))
        else:
            dvs.runcmd("ip neigh del {} dev {}".format(ips[i], intfs[i%2]))
            dvs.runcmd("ip neigh add {} dev {} lladdr {}".format(ips[i], intfs[1-i%2], macs[i]))

    for i in range(len(v6ips)):
        if i % 2 == 0:
            dvs.runcmd("ip -6 neigh change {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], newmacs[i]))
        else:
            dvs.runcmd("ip -6 neigh del {} dev {}".format(v6ips[i], intfs[i%2]))
            dvs.runcmd("ip -6 neigh add {} dev {} lladdr {}".format(v6ips[i], intfs[1-i%2], macs[i]))

    # start neighsyncd again
    start_neighsyncd(dvs)
    time.sleep(10)

    # timer is not expired yet, state should be "restored"
    swss_app_check_warmstart_state(state_db, "neighsyncd", "restored")
    time.sleep(10)

    # check neigh syncd timer is retrived from configDB
    check_neighsyncd_timer(dvs, timer_value)

    # check ipv4 and ipv6 neighbors, should see all neighbors with updated info
    for i in range(len(ips)):
        if i % 2 == 0:
            (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], ips[i]))
            assert status == True
            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == newmacs[i]
                if v[0] == "family":
                    assert v[1] == "IPv4"
        else:
            (status, fvs) = tbl.get("{}:{}".format(intfs[1-i%2], ips[i]))
            assert status == True
            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == macs[i]
                if v[0] == "family":
                    assert v[1] == "IPv4"

    for i in range(len(v6ips)):
        if i % 2 == 0:
            (status, fvs) = tbl.get("{}:{}".format(intfs[i%2], v6ips[i]))
            assert status == True
            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == newmacs[i]
                if v[0] == "family":
                    assert v[1] == "IPv6"
        else:
            (status, fvs) = tbl.get("{}:{}".format(intfs[1-i%2], v6ips[i]))
            assert status == True
            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == macs[i]
                if v[0] == "family":
                    assert v[1] == "IPv6"

    time.sleep(2)

    # check syslog and asic db for activities
    # 4 news, 2 deletes for ipv4 and ipv6 each
    # 4 create, 4 set, 4 removes for neighbor in asic db
    check_syslog_for_neighbor_entry(dvs, marker, 4, 2, "ipv4")
    check_syslog_for_neighbor_entry(dvs, marker, 4, 2, "ipv6")
    (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
    assert nadd == 8
    assert ndel == 4

    # check restore Count
    swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")


# TODO: The condition of warm restart readiness check is still under discussion.
def test_OrchagentWarmRestartReadyCheck(dvs, testlog):

    # do a pre-cleanup
    dvs.runcmd("ip -s -s neigh flush all")
    time.sleep(1)

    dvs.runcmd("config warm_restart enable swss")

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
    intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")
    dvs.runcmd("ifconfig Ethernet4 up")

    dvs.servers[0].runcmd("ifconfig eth0 10.0.0.1/31")
    dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

    dvs.servers[1].runcmd("ifconfig eth0 10.0.0.3/31")
    dvs.servers[1].runcmd("ip route add default via 10.0.0.2")


    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    ps = swsscommon.ProducerStateTable(appl_db, swsscommon.APP_ROUTE_TABLE_NAME)
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

    ps.set("2.2.2.0/24", fvs)

    time.sleep(1)
    # Should fail, since neighbor for next 10.0.0.1 has not been not resolved yet
    (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check")
    assert result == "RESTARTCHECK failed\n"

    # Should succeed, the option for skipPendingTaskCheck -s and noFreeze -n have been provided.
    # Wait up to 500 milliseconds for response from orchagent. Default wait time is 1000 milliseconds.
    (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check -n -s -w 500")
    assert result == "RESTARTCHECK succeeded\n"

    # get neighbor and arp entry
    dvs.servers[1].runcmd("ping -c 1 10.0.0.1")

    time.sleep(1)
    (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check")
    assert result == "RESTARTCHECK succeeded\n"

    # Should fail since orchagent has been frozen at last step.
    (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check -n -s -w 500")
    assert result == "RESTARTCHECK failed\n"

    # recover for test cases after this one.
    dvs.stop_swss()
    dvs.start_swss()
    time.sleep(5)

def test_swss_port_state_syncup(dvs, testlog):

    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    dvs.runcmd("config warm_restart enable swss")

    tbl = swsscommon.Table(appl_db, swsscommon.APP_PORT_TABLE_NAME)

    restore_count = swss_get_RestoreCount(state_db)

    # update port admin state
    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
    intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
    intf_tbl.set("Ethernet8|10.0.0.4/31", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")
    dvs.runcmd("ifconfig Ethernet4 up")
    dvs.runcmd("ifconfig Ethernet8 up")

    dvs.runcmd("arp -s 10.0.0.1 00:00:00:00:00:01")
    dvs.runcmd("arp -s 10.0.0.3 00:00:00:00:00:02")
    dvs.runcmd("arp -s 10.0.0.5 00:00:00:00:00:03")

    dvs.servers[0].runcmd("ip link set down dev eth0") == 0
    dvs.servers[1].runcmd("ip link set down dev eth0") == 0
    dvs.servers[2].runcmd("ip link set down dev eth0") == 0

    dvs.servers[2].runcmd("ip link set up dev eth0") == 0

    time.sleep(3)

    for i in [0, 1, 2]:
        (status, fvs) = tbl.get("Ethernet%d" % (i * 4))
        assert status == True
        oper_status = "unknown"
        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break
        if i == 2:
            assert oper_status == "up"
        else:
            assert oper_status == "down"

    dvs.stop_swss()
    time.sleep(3)

    # flap the port oper status for Ethernet0, Ethernet4 and Ethernet8
    dvs.servers[0].runcmd("ip link set down dev eth0") == 0
    dvs.servers[1].runcmd("ip link set down dev eth0") == 0
    dvs.servers[2].runcmd("ip link set down dev eth0") == 0

    dvs.servers[0].runcmd("ip link set up dev eth0") == 0
    dvs.servers[1].runcmd("ip link set up dev eth0") == 0

    time.sleep(5)
    dvs.start_swss()
    time.sleep(10)

    swss_check_RestoreCount(state_db, restore_count)

    for i in [0, 1, 2]:
        (status, fvs) = tbl.get("Ethernet%d" % (i * 4))
        assert status == True
        oper_status = "unknown"
        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break
        if i == 2:
            assert oper_status == "down"
        else:
            assert oper_status == "up"
