from swsscommon import swsscommon
import os
import re
import time
import json

# Get restore count of all processes supporting warm restart
def swss_get_RestoreCount(dvs, state_db):
    restore_count = {}
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    assert  len(keys) !=  0
    for key in keys:
        if key not in dvs.swssd:
            continue
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restore_count":
                restore_count[key] = int(fv[1])
    print(restore_count)
    return restore_count

# function to check the restore count incremented by 1 for all processes supporting warm restart
def swss_check_RestoreCount(dvs, state_db, restore_count):
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    print(keys)
    assert  len(keys) > 0
    for key in keys:
        if key not in dvs.swssd:
            continue
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

    dvs.runcmd("ifconfig Ethernet16 11.0.0.1/29 up")
    dvs.runcmd("ifconfig Ethernet20 11.0.0.9/29 up")

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

    restore_count = swss_get_RestoreCount(dvs, state_db)

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

    restore_count = swss_get_RestoreCount(dvs, state_db)

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

    dvs.runcmd("ifconfig {} 24.0.0.1/24 up".format(intfs[0]))
    dvs.runcmd("ip -6 addr add 2400::1/64 dev {}".format(intfs[0]))

    dvs.runcmd("ifconfig {} 28.0.0.1/24 up".format(intfs[1]))
    dvs.runcmd("ip -6 addr add 2800::1/64 dev {}".format(intfs[1]))

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
    restore_count = swss_get_RestoreCount(dvs, state_db)

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
    restore_count = swss_get_RestoreCount(dvs, state_db)

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
    restore_count = swss_get_RestoreCount(dvs, state_db)

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
    restore_count = swss_get_RestoreCount(dvs, state_db)

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

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up")

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

    restore_count = swss_get_RestoreCount(dvs, state_db)

    # update port admin state
    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up")
    dvs.runcmd("ifconfig Ethernet8 10.0.0.4/31 up")

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

    swss_check_RestoreCount(dvs, state_db, restore_count)

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


#############################################################################
#                                                                           #
#                        Routing Warm-Reboot Testing                        #
#                                                                           #
#############################################################################


def clear_logs(dvs):
    dvs.runcmd(['sh', '-c', '> /var/log/syslog'])
    dvs.runcmd(['sh', '-c', '> /var/log/swss/swss.rec'])
    dvs.runcmd(['sh', '-c', '> /var/log/swss/sairedis.rec'])


def start_zebra(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start zebra'])

    # Let's give zebra a chance to connect to FPM.
    time.sleep(5)


def stop_zebra(dvs):
    dvs.runcmd(['sh', '-c', 'pkill -x zebra'])
    time.sleep(1)


def start_fpmsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start fpmsyncd'])

    # Let's give zebra a chance to connect to Zebra.
    time.sleep(5)


def stop_fpmsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'pkill -x fpmsyncd'])
    time.sleep(1)


def enable_warmrestart(dvs, db, app_name):
    create_entry_tbl(
        db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, app_name,
        [
            ("enable", "true"),
        ]
    )


def disable_warmrestart(dvs, db, app_name):
    create_entry_tbl(
        db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, app_name,
        [
            ("enable", "false"),
        ]
    )


def set_restart_timer(dvs, db, app_name, value):
    create_entry_tbl(
        db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, app_name,
        [
            (app_name + "_timer", value),
        ]
    )


# function to check neighbor entry reconciliation status written in syslog
def check_syslog_change(dvs, entry, action):#new_cnt, delete_cnt, iptype):
    (exitcode, buf) = dvs.runcmd(['sh', '-c', 'egrep entry /var/log/syslog'])
    assert buf.find(action) != -1


#
# Verifies that a single change matching the passed parameters has been seen in
# swss logs.
#
# Input parameters (example):
#
#       table:   ROUTE
#       entry:   192.168.1.0/30
#       action:  SET, CREATE, DELETE
#       attribs: { nexthop:10.1.2.1,10.2.2.1,10.3.2.1|ifname:Ethernet112,Ethernet116,Ethernet120 }
#
# The above example would match this swss log entry:
#
# .* ROUTE_TABLE:192.168.1.0/30|SET|nexthop:10.1.2.1,10.2.2.1,10.3.2.1|ifname:Ethernet112,Ethernet116,Ethernet120 .*
#
def check_swss_change(dvs, table, entry, action, attribs):

    if action == 'SET':
        (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/swss.rec | wc -l".
                                      format(table + ".*" + entry + ".*" + "SET\|" + attribs)])

    elif action == 'CREATE':
        (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/swss.rec | wc -l".
                                      format(table + ".*" + entry + ".*" + "CREATE\|" + attribs)])

    elif action == 'DELETE':
        (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/swss.rec | wc -l".
                                      format(table + ".*" + entry + ".*" + "DEL")])

    print exitcode
    print num

    assert num.strip() == "1"


#
# Verifies that a single change matching the passed parameters has been seen in
# sairedis logs.
#
# Input parameters (example):
#
#       table: ROUTE
#       entry: 192.168.200.0/24
#       action: SET, CREATE, DELETE
#
# The above example would match this sairedis log entry:
#
# .* |s|SAI_OBJECT_TYPE_ROUTE_ENTRY:{"dest":"192.168.1.0/30" .*
#
def check_sairedis_change(dvs, table, entry, action):

    if action == 'SET':
        (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/sairedis.rec | wc -l".
                                      format("s\|" + ".*" + table + ".*" + entry)])

    elif action == 'CREATE':
        (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/sairedis.rec | wc -l".
                                      format("c\|" + ".*" + table + ".*" + entry)])

    elif action == 'DELETE':
        (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/sairedis.rec | wc -l".
                                      format("r\|" + ".*" + table + ".*" + entry)])

    assert num.strip() == "1"


def obtain_syslog_changes(dvs):

    (exitcode, num) = dvs.runcmd(['sh', '-c',
                                  'egrep "reconciliation:" /var/log/syslog | grep -v "no changes" | wc -l'])
    return num.strip()


#
# Obtain the total number of changes observed in swss.rec for a particular table.
#
def obtain_swss_changes(dvs, table):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/swss.rec | wc -l".
                                  format(table + "_TABLE")])
    return num.strip()


#
# Obtain the total number of changes observed in sairedis.rec for a particular table.
#
def obtain_sairedis_changes(dvs, table):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "egrep \"{}\" /var/log/swss/sairedis.rec | wc -l".
                                  format("TYPE_" + table + "_ENTRY")])
    return num.strip()


def check_restart_timer(dvs, timer_value):
    (exitcode, num) = dvs.runcmd(['sh', '-c',
                                  'grep "Getting warmStartTimer" /var/log/syslog | rev | cut -d ' ' -f 1 | rev'])
    assert num.strip() == timer_value


################################################################################
#
# Routing warm-restart testcases
#
################################################################################

def test_routing_WarmRestart(dvs):

    # Let's wait a few seconds for system to initialize
    time.sleep(60)

    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    restart_timer = 10


    #############################################################################
    #
    # Baseline configuration
    #
    #############################################################################


    # Defining create neighbor entries (4 ipv4 and 4 ip6, two each on each interface) in linux kernel
    intfs = ["Ethernet0", "Ethernet4", "Ethernet8"]

    # Enable ipv6 on docker
    dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")

    dvs.runcmd("ifconfig {} 11.0.0.1/24 up".format(intfs[0]))
    dvs.runcmd("ip -6 addr add 1100::1/64 dev {}".format(intfs[0]))

    dvs.runcmd("ifconfig {} 12.0.0.1/24 up".format(intfs[1]))
    dvs.runcmd("ip -6 addr add 1200::1/64 dev {}".format(intfs[1]))

    dvs.runcmd("ifconfig {} 13.0.0.1/24 up".format(intfs[2]))
    dvs.runcmd("ip -6 addr add 1300::1/64 dev {}".format(intfs[2]))

    time.sleep(1)

    #
    # Setting peer's ip-addresses and associated neighbor-entries
    #
    ips = ["11.0.0.2", "12.0.0.2", "13.0.0.2"]
    v6ips = ["1100::2", "1200::2", "1300::2"]
    macs = ["00:00:00:00:11:02", "00:00:00:00:12:02", "00:00:00:00:13:02"]

    for i in range(len(ips)):
        dvs.runcmd("ip neigh add {} dev {} lladdr {}".format(ips[i], intfs[i%2], macs[i]))

    for i in range(len(v6ips)):
        dvs.runcmd("ip -6 neigh add {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], macs[i]))

    time.sleep(1)

    #
    # Defining baseline IPv4 non-ecmp route-entries
    #
    dvs.runcmd("ip route add 192.168.1.100/32 nexthop via 11.0.0.2")
    dvs.runcmd("ip route add 192.168.1.200/32 nexthop via 12.0.0.2")
    dvs.runcmd("ip route add 192.168.1.300/32 nexthop via 13.0.0.2")

    #
    # Defining baseline IPv4 ecmp route-entries
    #
    dvs.runcmd("ip route add 192.168.1.1/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2 nexthop via 13.0.0.2")
    dvs.runcmd("ip route add 192.168.1.2/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2 nexthop via 13.0.0.2")
    dvs.runcmd("ip route add 192.168.1.3/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2")

    #
    # Defining baseline IPv6 non-ecmp route-entries
    #
    dvs.runcmd("ip -6 route add fc00:11:11::1/128 nexthop via 1100::2")
    dvs.runcmd("ip -6 route add fc00:12:12::1/128 nexthop via 1200::2")
    dvs.runcmd("ip -6 route add fc00:13:13::1/128 nexthop via 1300::2")

    #
    # Defining baseline IPv6 ecmp route-entries
    #
    dvs.runcmd("ip -6 route add fc00:1:1::1/128 nexthop via 1100::2 nexthop via 1200::2 nexthop via 1300::2")
    dvs.runcmd("ip -6 route add fc00:2:2::1/128 nexthop via 1100::2 nexthop via 1200::2 nexthop via 1300::2")
    dvs.runcmd("ip -6 route add fc00:3:3::1/128 nexthop via 1100::2 nexthop via 1200::2")

    time.sleep(5)

    dvs.runcmd("swssloglevel -l INFO -c fpmsyncd")


    #############################################################################
    #
    # Testcase 1. Having routing-warm-reboot disabled, restart zebra and verify
    #             that the traditional/cold-boot logic is followed.
    #
    #############################################################################

    # Enabling bgp warmrestart and setting restart timer.
    clear_logs(dvs)
    stop_zebra(dvs)
    start_zebra(dvs)

    time.sleep(5)

    # Verify that multiple changes are seen in swss and sairedis logs as there's
    # no warm-reboot logic in place.
    assert obtain_swss_changes(dvs, "ROUTE") != "0"
    assert obtain_sairedis_changes(dvs, "ROUTE") != "0"


    #############################################################################
    #
    # Testcase 2. Restart zebra and make no control-plane changes.
    #             For this and all subsequent test-cases routing-warm-reboot
    #             feature will be kept enabled.
    #
    #############################################################################


    # Enabling bgp warmrestart and setting restart timer.
    enable_warmrestart(dvs, conf_db, "bgp")
    set_restart_timer(dvs, conf_db, "bgp", str(restart_timer))
    time.sleep(1)

    # Clear logs and restart zebra
    clear_logs(dvs)
    stop_zebra(dvs)
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify swss/sairedis changes -- none are expected this time
    assert obtain_swss_changes(dvs, "ROUTE") == "0"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "0"


    #############################################################################
    #
    # Testcase 3. Restart zebra and add one new non-ecmp IPv4 prefix
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Add new prefix
    dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 11.0.0.2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "192.168.100.0/24", "SET", "nexthop:11.0.0.2|ifname:Ethernet0")
    check_sairedis_change(dvs, "ROUTE", "192.168.100.0/24", "CREATE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 4. Restart zebra and withdraw one non-ecmp IPv4 prefix
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Delete prefix
    dvs.runcmd("ip route del 192.168.100.0/24 nexthop via 11.0.0.2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "192.168.100.0/24", "DELETE", "")
    check_sairedis_change(dvs, "ROUTE", "192.168.100.0/24", "DELETE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 5. Restart zebra and add a new IPv4 ecmp-prefix
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Add prefix
    dvs.runcmd("ip route add 192.168.200.0/24 nexthop via 11.0.0.2 nexthop via 12.0.0.2 nexthop via 13.0.0.2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs,
                      "ROUTE",
                      "192.168.200.0/24",
                      "SET",
                      "nexthop:11.0.0.2,12.0.0.2,13.0.0.2|ifname:Ethernet0,Ethernet1,Ethernet2")

    check_sairedis_change(dvs, "ROUTE", "192.168.200.0/24", "CREATE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 6. Restart zebra and delete one existing IPv4 ecmp-prefix.
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Delete prefix
    dvs.runcmd("ip route del 192.168.200.0/24 nexthop via 11.0.0.2 nexthop via 12.0.0.2 nexthop via 13.0.0.2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "192.168.200.0/24", "DELETE", "")
    check_sairedis_change(dvs, "ROUTE", "192.168.200.0/24", "DELETE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 7. Restart zebra and add one new path to an IPv4 ecmp-prefix
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Add new path
    dvs.runcmd("ip route del 192.168.1.3/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2")
    dvs.runcmd("ip route add 192.168.1.3/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2 nexthop via 13.0.0.2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs,
                      "ROUTE",
                      "192.168.1.3/32",
                      "SET",
                      "nexthop:11.0.0.2,12.0.0.2,13.0.0.2|ifname:Ethernet0,Ethernet4,Ethernet8")

    check_sairedis_change(dvs, "ROUTE", "192.168.1.3/32", "SET")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 8. Restart zebra and delete one ecmp-path from an IPv4 ecmp-prefix.
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Delete ecmp-path
    dvs.runcmd("ip route del 192.168.1.3/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2 nexthop via 13.0.0.2")
    dvs.runcmd("ip route add 192.168.1.3/32 nexthop via 11.0.0.2 nexthop via 12.0.0.2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs,
                      "ROUTE",
                      "192.168.1.3/32",
                      "SET",
                      "nexthop:11.0.0.2,12.0.0.2|ifname:Ethernet0,Ethernet4")

    check_sairedis_change(dvs, "ROUTE", "192.168.1.3/32", "SET")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 9. Restart zebra and add one new non-ecmp IPv6 prefix
    #
    #############################################################################


    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Add prefix
    dvs.runcmd("ip -6 route add fc00:4:4::1/128 nexthop via 1100::2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "fc00:4:4::1", "SET", "nexthop:1100::2|ifname:Ethernet0")
    check_sairedis_change(dvs, "ROUTE", "fc00:4:4::1/128", "CREATE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 10. Restart zebra and withdraw one non-ecmp IPv6 prefix
    #
    #############################################################################

    # Clear logs and stop zebra
    clear_logs(dvs)
    stop_zebra(dvs)

    # Delete prefix
    dvs.runcmd("ip -6 route del fc00:4:4::1/128 nexthop via 1100::2")
    time.sleep(1)

    # Start zebra
    start_zebra(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "fc00:4:4::1", "DELETE", "")
    check_sairedis_change(dvs, "ROUTE", "fc00:4:4::1/128", "DELETE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 11. Restart fpmsyncd and make no control-plane changes.
    #
    #############################################################################


    # Clear logs and restart fpmsyncd
    clear_logs(dvs)
    stop_fpmsyncd(dvs)
    start_fpmsyncd(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify swss/sairedis changes -- none are expected this time
    assert obtain_swss_changes(dvs, "ROUTE") == "0"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "0"


    #############################################################################
    #
    # Testcase 12. Restart fpmsyncd and add one new non-ecmp IPv4 prefix
    #
    #############################################################################


    # Clear logs and stop fpmsyncd
    clear_logs(dvs)
    stop_fpmsyncd(dvs)

    # Add new prefix
    dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 11.0.0.2")
    time.sleep(1)

    # Start fpmsyncd
    start_fpmsyncd(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "192.168.100.0/24", "SET", "nexthop:11.0.0.2|ifname:Ethernet0")
    check_sairedis_change(dvs, "ROUTE", "192.168.100.0/24", "CREATE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"


    #############################################################################
    #
    # Testcase 13. Restart fpmsyncd and withdraw one non-ecmp IPv4 prefix
    #
    #############################################################################


    # Clear logs and stop fpmsyncd
    clear_logs(dvs)
    stop_fpmsyncd(dvs)

    # Delete prefix
    dvs.runcmd("ip route del 192.168.100.0/24 nexthop via 11.0.0.2")
    time.sleep(1)

    # Start fpmsyncd
    start_fpmsyncd(dvs)

    # Verify FSM
    swss_app_check_warmstart_state(state_db, "bgp", "restored")
    time.sleep(restart_timer + 1)
    swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

    # Verify the changed prefix is seen in swss/sairedis
    check_swss_change(dvs, "ROUTE", "192.168.100.0/24", "DELETE", "")
    check_sairedis_change(dvs, "ROUTE", "192.168.100.0/24", "DELETE")

    # Verify swss/sairedis changes -- a single one is expected
    assert obtain_swss_changes(dvs, "ROUTE") == "1"
    assert obtain_sairedis_changes(dvs, "ROUTE") == "1"
