from swsscommon import swsscommon
import os
import re
import time
import json

# start processes in SWSS
def start_swss(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start orchagent; supervisorctl start portsyncd; supervisorctl start intfsyncd; \
        supervisorctl start neighsyncd; supervisorctl start intfmgrd; supervisorctl start vlanmgrd; \
        supervisorctl start buffermgrd; supervisorctl start arp_update'])

# stop processes in SWSS
def stop_swss(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl stop orchagent; supervisorctl stop portsyncd; supervisorctl stop intfsyncd; \
        supervisorctl stop neighsyncd;  supervisorctl stop intfmgrd; supervisorctl stop vlanmgrd; \
        supervisorctl stop buffermgrd; supervisorctl stop arp_update'])


# Get restart count of all processes supporting warm restart
def swss_get_RestartCount(state_db):
    restart_count = {}
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    assert  len(keys) !=  0
    for key in keys:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restart_count":
                restart_count[key] = int(fv[1])
    print(restart_count)
    return restart_count

# function to check the restart count incremented by 1 for all processes supporting warm restart
def swss_check_RestartCount(state_db, restart_count):
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    print(keys)
    assert  len(keys) > 0
    for key in keys:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restart_count":
                assert int(fv[1]) == restart_count[key] + 1
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

# function to check the restart count incremented by 1 for a single process
def swss_app_check_RestartCount_single(state_db, restart_count, name):
    warmtbl = swsscommon.Table(state_db, swsscommon.STATE_WARM_RESTART_TABLE_NAME)
    keys = warmtbl.getKeys()
    print(keys)
    print(restart_count)
    assert  len(keys) > 0
    for key in keys:
        if key != name:
            continue
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restart_count":
                assert int(fv[1]) == restart_count[key] + 1
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

# No create/set/remove operations should be passed down to syncd for vlanmgr/portsyncd warm restart
def checkCleanSaiRedisCSR(dvs):
    (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep \|c\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'
    (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep \|s\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'
    (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep \|r\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'

def test_PortSyncdWarmRestart(dvs):

    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    # enable warm restart
    # TODO: use cfg command to config it
    create_entry_tbl(
        conf_db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
        [
            ("enable", "true"),
        ]
    )

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

    restart_count = swss_get_RestartCount(state_db)

    # restart portsyncd
    dvs.runcmd(['sh', '-c', 'pkill -x portsyncd; cp /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b; echo > /var/log/swss/sairedis.rec'])
    dvs.runcmd(['sh', '-c', 'supervisorctl start portsyncd'])
    time.sleep(2)

    checkCleanSaiRedisCSR(dvs)

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


    swss_app_check_RestartCount_single(state_db, restart_count, "portsyncd")


def test_VlanMgrdWarmRestart(dvs):

    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    dvs.runcmd("ifconfig Ethernet16  0")
    dvs.runcmd("ifconfig Ethernet20  0")

    dvs.runcmd("ifconfig Ethernet16  up")
    dvs.runcmd("ifconfig Ethernet20  up")

    time.sleep(1)

    # enable warm restart
    # TODO: use cfg command to config it
    create_entry_tbl(
        conf_db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
        [
            ("enable", "true"),
        ]
    )

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

    restart_count = swss_get_RestartCount(state_db)

    dvs.runcmd(['sh', '-c', 'pkill -x vlanmgrd; cp /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b; echo > /var/log/swss/sairedis.rec'])
    dvs.runcmd(['sh', '-c', 'supervisorctl start vlanmgrd'])
    time.sleep(2)

    (exitcode, bv_after) = dvs.runcmd("bridge vlan")
    assert bv_after == bv_before

    checkCleanSaiRedisCSR(dvs)

    #new ip on server 5
    dvs.servers[5].runcmd("ifconfig eth0 11.0.0.11/29")

    # Ping should work between servers via vs vlan interfaces
    ping_stats = dvs.servers[4].runcmd("ping -c 1 11.0.0.11")

    # new neighbor learn on VS
    (status, fvs) = tbl.get("Vlan20:11.0.0.11")
    assert status == True

    swss_app_check_RestartCount_single(state_db, restart_count, "vlanmgrd")

# function to stop neighsyncd service and clear syslog and sairedis records
def stop_neighsyncd_clear_syslog_sairedis(dvs, save_number):
    dvs.runcmd(['sh', '-c', 'pkill -x neighsyncd'])
    dvs.runcmd("cp /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.back{}".format(save_number))
    dvs.runcmd(['sh', '-c', '> /var/log/swss/sairedis.rec'])
    dvs.runcmd("cp /var/log/syslog /var/log/syslog.back{}".format(save_number))
    dvs.runcmd(['sh', '-c', '> /var/log/syslog'])

def start_neighsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start neighsyncd'])

def check_no_neighsyncd_timer(dvs):
    (exitcode, string) = dvs.runcmd(['sh', '-c', 'grep getWarmStartTimer /var/log/syslog | grep neighsyncd | grep invalid'])
    assert string.strip() != ""

def check_neighsyncd_timer(dvs, timer_value):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "grep getWarmStartTimer /var/log/syslog | grep neighsyncd | rev | cut -d ' ' -f 1 | rev"])
    assert num.strip() == timer_value

# function to check neighbor entry reconciliation status written in syslog
def check_syslog_for_neighbor_entry(dvs, new_cnt, delete_cnt, iptype):
    # check reconciliation results (new or delete entries) for ipv4 and ipv6
    if iptype == "ipv4":
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep neighsyncd /var/log/syslog| grep cache-state:NEW | grep IPv4 | wc -l'])
        assert num.strip() == str(new_cnt)
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep neighsyncd /var/log/syslog| grep cache-state:DELETE | grep IPv4 | wc -l'])
        assert num.strip() == str(delete_cnt)
    elif iptype == "ipv6":
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep neighsyncd /var/log/syslog| grep cache-state:NEW | grep IPv6 | wc -l'])
        assert num.strip() == str(new_cnt)
        (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep neighsyncd /var/log/syslog| grep cache-state:DELETE | grep IPv6 | wc -l'])
        assert num.strip() == str(delete_cnt)
    else:
        assert "iptype is unknown" == ""


# function to check sairedis record for neighbor entries
def check_sairedis_for_neighbor_entry(dvs, create_cnt, set_cnt, remove_cnt):
    # check create/set/remove operations for neighbor entries during warm restart
    (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep \|c\| /var/log/swss/sairedis.rec | grep NEIGHBOR_ENTRY | wc -l'])
    assert num.strip() == str(create_cnt)
    (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep \|s\| /var/log/swss/sairedis.rec | grep NEIGHBOR_ENTRY | wc -l'])
    assert num.strip() == str(set_cnt)
    (exitcode, num) = dvs.runcmd(['sh', '-c', 'grep \|r\| /var/log/swss/sairedis.rec | grep NEIGHBOR_ENTRY | wc -l'])
    assert num.strip() == str(remove_cnt)


def test_swss_neighbor_syncup(dvs):

    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    # enable warm restart
    # TODO: use cfg command to config it
    create_entry_tbl(
        conf_db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
        [
            ("enable", "true"),
        ]
    )

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

    # get restart_count
    restart_count = swss_get_RestartCount(state_db)

    # stop neighsyncd and clear syslog and sairedis.rec
    stop_neighsyncd_clear_syslog_sairedis(dvs, 1)

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
    check_syslog_for_neighbor_entry(dvs, 0, 0, "ipv4")
    check_syslog_for_neighbor_entry(dvs, 0, 0, "ipv6")
    check_sairedis_for_neighbor_entry(dvs, 0, 0, 0)

    # check restart Count
    swss_app_check_RestartCount_single(state_db, restart_count, "neighsyncd")

    #
    # Testcase 3:
    # stop neighsyncd, delete even nummber ipv4/ipv6 neighbor entries from each interface, warm start neighsyncd.
    # the neighsyncd is supposed to sync up the entries from kernel after warm restart
    # note: there was an issue for neighbor delete, it will be marked as FAILED instead of deleted in kernel
    #       but it will send netlink message to be removed from appDB, so it works ok here,
    #       just that if we want to add the same neighbor again, use "change" instead of "add"

    # get restart_count
    restart_count = swss_get_RestartCount(state_db)

    # stop neighsyncd and clear syslog and sairedis.rec
    stop_neighsyncd_clear_syslog_sairedis(dvs, 2)

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
    # 4 remove actions in sairedis
    check_syslog_for_neighbor_entry(dvs, 0, 2, "ipv4")
    check_syslog_for_neighbor_entry(dvs, 0, 2, "ipv6")
    check_sairedis_for_neighbor_entry(dvs, 0, 0, 4)
    # check restart Count
    swss_app_check_RestartCount_single(state_db, restart_count, "neighsyncd")


    #
    # Testcase 4:
    # Stop neighsyncd, add even nummber of ipv4/ipv6 neighbor entries to each interface again,
    # use "change" due to the kernel behaviour, start neighsyncd.
    # The neighsyncd is supposed to sync up the entries from kernel after warm restart
    # Check the timer is not retrieved from configDB since it is not configured

    # get restart_count
    restart_count = swss_get_RestartCount(state_db)

    # stop neighsyncd and clear syslog and sairedis.rec
    stop_neighsyncd_clear_syslog_sairedis(dvs, 3)

    # add even nummber of ipv4/ipv6 neighbor entries to each interface
    for i in range(0, len(ips), 2):
        dvs.runcmd("ip neigh change {} dev {} lladdr {}".format(ips[i], intfs[i%2], macs[i]))

    for i in range(0, len(v6ips), 2):
        dvs.runcmd("ip -6 neigh change {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], macs[i]))

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

    # check syslog and sairedis.rec file for activities
    # 2 news entries for ipv4 and ipv6 each
    # 4 create actions for sairedis
    check_syslog_for_neighbor_entry(dvs, 2, 0, "ipv4")
    check_syslog_for_neighbor_entry(dvs, 2, 0, "ipv6")
    check_sairedis_for_neighbor_entry(dvs, 4, 0, 0)
    # check restart Count
    swss_app_check_RestartCount_single(state_db, restart_count, "neighsyncd")

    #
    # Testcase 5:
    # Even number of ip4/6 neigbors updated with new mac.
    # Odd number of ipv4/6 neighbors removed and added to different interfaces.
    # neighbor syncd should sync it up after warm restart
    # include the timer settings in this testcase

    # setup timer in configDB
    timer_value = "15"

    create_entry_tbl(
        conf_db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
        [
            ("neighsyncd_timer", timer_value),
        ]
    )

    # get restart_count
    restart_count = swss_get_RestartCount(state_db)

    # stop neighsyncd and clear syslog and sairedis.rec
    stop_neighsyncd_clear_syslog_sairedis(dvs, 4)

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

    # check syslog and sairedis.rec file for activities
    # 4 news, 2 deletes for ipv4 and ipv6 each
    # 8 create, 4 set, 4 removes for sairedis
    check_syslog_for_neighbor_entry(dvs, 4, 2, "ipv4")
    check_syslog_for_neighbor_entry(dvs, 4, 2, "ipv6")
    check_sairedis_for_neighbor_entry(dvs, 4, 4, 4)
    # check restart Count
    swss_app_check_RestartCount_single(state_db, restart_count, "neighsyncd")


# TODO: The condition of warm restart readiness check is still under discussion.
def test_OrchagentWarmRestartReadyCheck(dvs):

    # do a pre-cleanup
    dvs.runcmd("ip -s -s neigh flush all")
    time.sleep(1)

    # enable warm restart
    # TODO: use cfg command to config it
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

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
    stop_swss(dvs)
    start_swss(dvs)
    time.sleep(5)

def test_swss_port_state_syncup(dvs):

    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

    # enable warm restart
    # TODO: use cfg command to config it
    create_entry_tbl(
        conf_db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
        [
            ("enable", "true"),
        ]
    )

    tbl = swsscommon.Table(appl_db, swsscommon.APP_PORT_TABLE_NAME)

    restart_count = swss_get_RestartCount(state_db)

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

    stop_swss(dvs)
    time.sleep(3)

    # flap the port oper status for Ethernet0, Ethernet4 and Ethernet8
    dvs.servers[0].runcmd("ip link set down dev eth0") == 0
    dvs.servers[1].runcmd("ip link set down dev eth0") == 0
    dvs.servers[2].runcmd("ip link set down dev eth0") == 0

    dvs.servers[0].runcmd("ip link set up dev eth0") == 0
    dvs.servers[1].runcmd("ip link set up dev eth0") == 0

    time.sleep(5)
    start_swss(dvs)
    time.sleep(10)

    swss_check_RestartCount(state_db, restart_count)

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

