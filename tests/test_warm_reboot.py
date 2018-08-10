from swsscommon import swsscommon
import os
import re
import time
import json

# Get restart count of all processes supporting warm restart
def swss_get_RestartCount(state_db):
    restart_count = {}
    warmtbl = swsscommon.Table(state_db, "WARM_RESTART_TABLE")
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
    warmtbl = swsscommon.Table(state_db, "WARM_RESTART_TABLE")
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
    portTbl = swsscommon.Table(appl_db, "PORT_TABLE")
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
    warmtbl = swsscommon.Table(state_db, "WARM_RESTART_TABLE")
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
        "WARM_RESTART", "swss",
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


    bv_before = dvs.runcmd("bridge vlan")
    print(bv_before)

    restart_count = swss_get_RestartCount(state_db)

    dvs.runcmd(['sh', '-c', 'pkill -x vlanmgrd; cp /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b; echo > /var/log/swss/sairedis.rec'])
    dvs.runcmd(['sh', '-c', 'supervisorctl start vlanmgrd'])
    time.sleep(2)

    bv_after = dvs.runcmd("bridge vlan")
    assert bv_after == bv_before

     # No create/set/remove operations should be passed down to syncd for vlanmgr warm restart
    num = dvs.runcmd(['sh', '-c', 'grep \|c\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'
    num = dvs.runcmd(['sh', '-c', 'grep \|s\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'
    num = dvs.runcmd(['sh', '-c', 'grep \|r\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'

    #new ip on server 5
    dvs.servers[5].runcmd("ifconfig eth0 11.0.0.11/29")

    # Ping should work between servers via vs vlan interfaces
    ping_stats = dvs.servers[4].runcmd("ping -c 1 11.0.0.11")

    # new neighbor learn on VS
    (status, fvs) = tbl.get("Vlan20:11.0.0.11")
    assert status == True

    swss_app_check_RestartCount_single(state_db, restart_count, "vlanmgrd")
