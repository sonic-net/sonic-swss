from swsscommon import swsscommon
import os
import re
import time
import json

<<<<<<< e03d6e9b5c886360cb245dbee69e417cad6996c8
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

# TODO: The condition of warm restart readiness check is still under discussion.
def test_OrchagentWarmRestartReadyCheck(dvs):

    # do a pre-cleanup
    dvs.runcmd("ip -s -s neigh flush all")
    time.sleep(1)

    dvs.runcmd("config warm_restart enable swss")
    # hostcfgd not running in VS, create the folder explicitly
    dvs.runcmd("mkdir -p /etc/sonic/warm_restart/swss")

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up")

    dvs.servers[0].runcmd("ifconfig eth0 10.0.0.1/31")
    dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

    dvs.servers[1].runcmd("ifconfig eth0 10.0.0.3/31")
    dvs.servers[1].runcmd("ip route add default via 10.0.0.2")

    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    ps = swsscommon.ProducerStateTable(appl_db, "ROUTE_TABLE")
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

    ps.set("2.2.2.0/24", fvs)

    time.sleep(1)
    # Should fail, since neighbor for next 10.0.0.1 has not been not resolved yet
    result =  dvs.runcmd("/usr/bin/orchagent_restart_check")
    assert result == "RESTARTCHECK failed\n"

    # Should succeed, the option for skipPendingTaskCheck -s and noFreeze -n have been provided.
    # Wait up to 500 milliseconds for response from orchagent. Default wait time is 1000 milliseconds.
    result =  dvs.runcmd("/usr/bin/orchagent_restart_check -n -s -w 500")
    assert result == "RESTARTCHECK succeeded\n"

    # get neighbor and arp entry
    dvs.servers[1].runcmd("ping -c 1 10.0.0.1")

    time.sleep(1)
    result =  dvs.runcmd("/usr/bin/orchagent_restart_check")
    assert result == "RESTARTCHECK succeeded\n"

    # Should fail since orchagent has been frozen at last step.
    result =  dvs.runcmd("/usr/bin/orchagent_restart_check -n -s -w 500")
    assert result == "RESTARTCHECK failed\n"
