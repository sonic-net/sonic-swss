import os
import re
import time
import json
import pytest

from swsscommon import swsscommon


# macros for number of interfaces and number of neighbors
# TBD: NUM_NEIGH_PER_INTF >= 128 ips will cause test framework to hang by default kernel settings
# TBD: Need tune gc_thresh1/2/3 at host side of vs docker to support this.
NUM_INTF = 8
NUM_NEIGH_PER_INTF = 16 #128
NUM_OF_NEIGHS = (NUM_INTF*NUM_NEIGH_PER_INTF)

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
                assert fv[1] == "reconciled" or fv[1] == "disabled"

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
                assert fv[1] == "reconciled" or fv[1] == "disabled"
    return status, fvs

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

def stop_neighsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'pkill -x neighsyncd'])

def start_neighsyncd(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start neighsyncd'])

def stop_restore_neighbors(dvs):
    dvs.runcmd(['sh', '-c', 'pkill -x restore_neighbors'])

def start_restore_neighbors(dvs):
    dvs.runcmd(['sh', '-c', 'supervisorctl start restore_neighbors'])

def check_no_neighsyncd_timer(dvs):
    (exitcode, string) = dvs.runcmd(['sh', '-c', 'grep getWarmStartTimer /var/log/syslog | grep neighsyncd | grep invalid'])
    assert string.strip() != ""

def check_neighsyncd_timer(dvs, timer_value):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "grep getWarmStartTimer /var/log/syslog | grep neighsyncd | tail -n 1 | rev | cut -d ' ' -f 1 | rev"])
    assert num.strip() == timer_value

def check_redis_neigh_entries(dvs, neigh_tbl, number):
    # check application database and get neighbor table
    appl_db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
    lo_entrytbl = swsscommon.Table(appl_db, "NEIGH_TABLE:lo")
    lo_entries = lo_entrytbl.getKeys()
    assert len(neigh_tbl.getKeys()) == number + len(lo_entries)

def check_kernel_reachable_neigh_num(dvs, number):
    (exitcode, output) = dvs.runcmd(['sh', '-c', "ip neigh show nud reachable| grep -v 'dev lo' | wc -l"])
    neigh_num = int(output.strip())
    assert neigh_num == number

def check_kernel_reachable_v4_neigh_num(dvs, number):
    (exitcode, output) = dvs.runcmd(['sh', '-c', "ip -4 neigh show nud reachable | grep -v 'dev lo' | wc -l"])
    neigh_num = int(output.strip())
    assert neigh_num == number

def check_kernel_reachable_v6_neigh_num(dvs, number):
    (exitcode, output) = dvs.runcmd(['sh', '-c', "ip -6 neigh show nud reachable | grep -v 'dev lo' | wc -l"])
    neigh_num = int(output.strip())
    assert neigh_num == number

def check_kernel_stale_neigh_num(dvs, number):
    (exitcode, output) = dvs.runcmd(['sh', '-c', "ip neigh show nud stale | grep -v 'dev lo' | wc -l"])
    neigh_num = int(output.strip())
    assert neigh_num == number

def check_kernel_stale_v4_neigh_num(dvs, number):
    (exitcode, output) = dvs.runcmd(['sh', '-c', "ip -4 neigh show nud stale | grep -v 'dev lo' | wc -l"])
    neigh_num = int(output.strip())
    assert neigh_num == number

def check_kernel_stale_v6_neigh_num(dvs, number):
    (exitcode, output) = dvs.runcmd(['sh', '-c', "ip -6 neigh show nud stale | grep -v 'dev lo' | wc -l"])
    neigh_num = int(output.strip())
    assert neigh_num == number


def kernel_restore_neighs_done(restoretbl):
    keys = restoretbl.getKeys()
    return (len(keys) > 0)

# function to check neighbor entry reconciliation status written in syslog
def check_syslog_for_neighbor_entry(dvs, marker, new_cnt, delete_cnt, iptype):
    # check reconciliation results (new or delete entries) for ipv4 and ipv6
    if iptype == "ipv4" or iptype == "ipv6":
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep neighsyncd | grep cache-state:NEW | grep -i %s | grep -v 'lo:'| wc -l" % (marker, iptype)])
        assert num.strip() == str(new_cnt)
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep neighsyncd | grep -E \"cache-state:(DELETE|STALE)\" | grep -i %s | grep -v 'lo:' | wc -l" % (marker, iptype)])
        assert num.strip() == str(delete_cnt)
    else:
        assert "iptype is unknown" == ""

def set_restart_timer(dvs, db, app_name, value):
    create_entry_tbl(
        db,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, app_name,
        [
            (app_name + "_timer", value),
        ]
    )

# 'ip neigh flush all' won't remove failed entries if number of neighs less than gc_threshold1
# Also it takes time to remove them completly.
# We use arp off/on to do it
def flush_neigh_entries(dvs):
    dvs.runcmd("ip link set group default arp off")
    dvs.runcmd("ip link set group default arp on")

# Add neighbor entries on servers connecting to SONiC ports
# ping them to get the neighbor entries
def setup_initial_neighbors(dvs):
    for i in range(8, 8+NUM_INTF):
        for j in range(NUM_NEIGH_PER_INTF):
            dvs.servers[i].runcmd("ip addr add {}.0.0.{}/24 dev eth0".format(i*4, j+2))
            dvs.servers[i].runcmd("ip -6 addr add {}00::{}/64 dev eth0".format(i*4, j+2))

    time.sleep(1)

    for i in range(8, 8+NUM_INTF):
        for j in range(NUM_NEIGH_PER_INTF):
            dvs.runcmd(['sh', '-c', "ping -c 1 -W 0 -q {}.0.0.{} > /dev/null 2>&1".format(i*4, j+2)])
            dvs.runcmd(['sh', '-c', "ping6 -c 1 -W 0 -q {}00::{} > /dev/null 2>&1".format(i*4, j+2)])

# Del half of the ips and a new half of the ips
# note: the first ipv4 can not be deleted only
def del_and_add_neighbors(dvs):
    for i in range(8, 8+NUM_INTF):
        for j in range(NUM_NEIGH_PER_INTF//2):
            dvs.servers[i].runcmd("ip addr del {}.0.0.{}/24 dev eth0".format(i*4, j+NUM_NEIGH_PER_INTF//2+2))
            dvs.servers[i].runcmd("ip -6 addr del {}00::{}/64 dev eth0".format(i*4, j+NUM_NEIGH_PER_INTF//2+2))
            dvs.servers[i].runcmd("ip addr add {}.0.0.{}/24 dev eth0".format(i*4, j+NUM_NEIGH_PER_INTF+2))
            dvs.servers[i].runcmd("ip -6 addr add {}00::{}/64 dev eth0".format(i*4, j+NUM_NEIGH_PER_INTF+2))

#ping new IPs
def ping_new_ips(dvs):
    for i in range(8, 8+NUM_INTF):
        for j in range(NUM_NEIGH_PER_INTF//2):
            dvs.runcmd(['sh', '-c', "ping -c 1 -W 0 -q {}.0.0.{} > /dev/null 2>&1".format(i*4, j+NUM_NEIGH_PER_INTF+2)])
            dvs.runcmd(['sh', '-c', "ping6 -c 1 -W 0 -q {}00::{} > /dev/null 2>&1".format(i*4, j+NUM_NEIGH_PER_INTF+2)])

def warm_restart_set(dvs, app, enable):
    db = swsscommon.DBConnector(6, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "WARM_RESTART_ENABLE_TABLE")
    fvs = swsscommon.FieldValuePairs([("enable",enable)])
    tbl.set(app, fvs)
    time.sleep(1)


def warm_restart_timer_set(dvs, app, timer, val):
    db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "WARM_RESTART")
    fvs = swsscommon.FieldValuePairs([(timer, val)])
    tbl.set(app, fvs)
    time.sleep(1)

class TestWarmReboot(object):
    def test_PortSyncdWarmRestart(self, dvs, testlog):

        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        dvs.warm_restart_swss("true")

        dvs.port_admin_set("Ethernet16", "up")
        dvs.port_admin_set("Ethernet20", "up")

        time.sleep(1)

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet16|11.0.0.1/29", fvs)
        intf_tbl.set("Ethernet20|11.0.0.9/29", fvs)
        intf_tbl.set("Ethernet16", fvs)
        intf_tbl.set("Ethernet20", fvs)
        dvs.port_admin_set("Ethernet16", "up")
        dvs.port_admin_set("Ethernet20", "up")

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
        check_port_oper_status(appl_db, "Ethernet24", "down")


        swss_app_check_RestoreCount_single(state_db, restore_count, "portsyncd")

        intf_tbl._del("Ethernet16|11.0.0.1/29")
        intf_tbl._del("Ethernet20|11.0.0.9/29")
        intf_tbl._del("Ethernet16")
        intf_tbl._del("Ethernet20")
        time.sleep(2)


    def test_VlanMgrdWarmRestart(self, dvs, testlog):

        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        dvs.runcmd("ifconfig Ethernet16  0")
        dvs.runcmd("ifconfig Ethernet20  0")

        dvs.port_admin_set("Ethernet16", "up")
        dvs.port_admin_set("Ethernet20", "up")

        time.sleep(1)

        dvs.warm_restart_swss("true")

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

        intf_tbl = swsscommon.Table(conf_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Vlan16|11.0.0.1/29", fvs)
        intf_tbl.set("Vlan20|11.0.0.9/29", fvs)
        intf_tbl.set("Vlan16", fvs)
        intf_tbl.set("Vlan20", fvs)

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

        intf_tbl._del("Vlan16|11.0.0.1/29")
        intf_tbl._del("Vlan20|11.0.0.9/29")
        intf_tbl._del("Vlan16")
        intf_tbl._del("Vlan20")
        time.sleep(2)

    def test_IntfMgrdWarmRestartNoInterfaces(self, dvs, testlog):
        """ Tests that intfmgrd reaches reconciled state when
        there are no interfaces in configuration. """

        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        restore_count = swss_get_RestoreCount(dvs, state_db)

        dvs.warm_restart_swss("true")
        dvs.runcmd("supervisorctl restart intfmgrd")

        reached_desired_state = False
        retries = 10
        delay = 2
        for _ in range(retries):
            ok, fvs = swss_app_check_RestoreCount_single(state_db, restore_count, "intfmgrd")
            if ok and dict(fvs)["state"] == "reconciled":
                reached_desired_state = True
                break
            time.sleep(delay)

        assert reached_desired_state, "intfmgrd haven't reached desired state 'reconciled', after {} sec it was {}".format(retries * delay, dict(fvs)["state"])

    def test_swss_neighbor_syncup(self, dvs, testlog):

        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        dvs.warm_restart_swss("true")

        #
        # Testcase1:
        # Add neighbor entries in linux kernel, appDB should get all of them
        #

        # create neighbor entries (4 ipv4 and 4 ip6, two each on each interface) in linux kernel
        intfs = ["Ethernet24", "Ethernet28"]

        for intf in intfs:
            # set timeout to be the same as real HW
            dvs.runcmd("sysctl -w net.ipv4.neigh.{}.base_reachable_time_ms=1800000".format(intf))
            dvs.runcmd("sysctl -w net.ipv6.neigh.{}.base_reachable_time_ms=1800000".format(intf))

        #enable ipv6 on docker
        dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("{}|24.0.0.1/24".format(intfs[0]), fvs)
        intf_tbl.set("{}|28.0.0.9/24".format(intfs[1]), fvs)
        intf_tbl.set("{}|2400::1/64".format(intfs[0]), fvs)
        intf_tbl.set("{}|2800::1/64".format(intfs[1]), fvs)
        intf_tbl.set("{}".format(intfs[0]), fvs)
        intf_tbl.set("{}".format(intfs[1]), fvs)
        intf_tbl.set("{}".format(intfs[0]), fvs)
        intf_tbl.set("{}".format(intfs[1]), fvs)
        dvs.port_admin_set(intfs[0], "up")
        dvs.port_admin_set(intfs[1], "up")

        ips = ["24.0.0.2", "24.0.0.3", "28.0.0.2", "28.0.0.3"]
        v6ips = ["2400::2", "2400::3", "2800::2", "2800::3"]

        macs = ["00:00:00:00:24:02", "00:00:00:00:24:03", "00:00:00:00:28:02", "00:00:00:00:28:03"]

        for i in range(len(ips)):
            dvs.runcmd("ip neigh add {} dev {} lladdr {} nud reachable".format(ips[i], intfs[i//2], macs[i]))

        for i in range(len(v6ips)):
            dvs.runcmd("ip -6 neigh add {} dev {} lladdr {} nud reachable".format(v6ips[i], intfs[i//2], macs[i]))

        time.sleep(1)

        # Check the neighbor entries are inserted correctly
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "NEIGH_TABLE")

        for i in range(len(ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], ips[i]))
            assert status == True

            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == macs[i]
                if v[0] == "family":
                    assert v[1] == "IPv4"

        for i in range(len(v6ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], v6ips[i]))
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
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        marker = dvs.add_log_marker()
        pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)
        time.sleep(10)

        # Check the neighbor entries are still in appDB correctly
        for i in range(len(ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], ips[i]))
            assert status == True

            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == macs[i]
                if v[0] == "family":
                    assert v[1] == "IPv4"

        for i in range(len(v6ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], v6ips[i]))
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
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        marker = dvs.add_log_marker()

        # delete even nummber of ipv4/ipv6 neighbor entries from each interface
        for i in range(0, len(ips), 2):
            dvs.runcmd("ip neigh del {} dev {}".format(ips[i], intfs[i//2]))

        for i in range(0, len(v6ips), 2):
            dvs.runcmd("ip -6 neigh del {} dev {}".format(v6ips[i], intfs[i//2]))

        # start neighsyncd again
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)
        time.sleep(10)

        # check ipv4 and ipv6 neighbors
        for i in range(len(ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], ips[i]))
            #should not see deleted neighbor entries
            if i % 2 == 0:
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
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], v6ips[i]))
            #should not see deleted neighbor entries
            if i % 2 == 0:
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
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        marker = dvs.add_log_marker()

        # add even nummber of ipv4/ipv6 neighbor entries to each interface
        # use "change" if neighbor is in FAILED state
        for i in range(0, len(ips), 2):
            (rc, output) = dvs.runcmd(['sh', '-c', "ip -4 neigh | grep {}".format(ips[i])])
            print(output)
            if output:
                dvs.runcmd("ip neigh change {} dev {} lladdr {} nud reachable".format(ips[i], intfs[i//2], macs[i]))
            else:
                dvs.runcmd("ip neigh add {} dev {} lladdr {} nud reachable".format(ips[i], intfs[i//2], macs[i]))

        for i in range(0, len(v6ips), 2):
            (rc, output) = dvs.runcmd(['sh', '-c', "ip -6 neigh | grep {}".format(v6ips[i])])
            print(output)
            if output:
                dvs.runcmd("ip -6 neigh change {} dev {} lladdr {} nud reachable".format(v6ips[i], intfs[i//2], macs[i]))
            else:
                dvs.runcmd("ip -6 neigh add {} dev {} lladdr {} nud reachable".format(v6ips[i], intfs[i//2], macs[i]))

        # start neighsyncd again
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)
        time.sleep(10)

        # no neighsyncd timer configured
        check_no_neighsyncd_timer(dvs)

        # check ipv4 and ipv6 neighbors, should see all neighbors
        for i in range(len(ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], ips[i]))
            assert status == True
            for v in fvs:
                if v[0] == "neigh":
                    assert v[1] == macs[i]
                if v[0] == "family":
                    assert v[1] == "IPv4"

        for i in range(len(v6ips)):
            (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], v6ips[i]))
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
        # Odd number of ipv4/6 neighbors removed
        # neighbor syncd should sync it up after warm restart
        # include the timer settings in this testcase

        # setup timer in configDB
        timer_value = "15"

        warm_restart_timer_set(dvs, "swss", "neighsyncd_timer", timer_value)

        # get restore_count
        restore_count = swss_get_RestoreCount(dvs, state_db)

        # stop neighsyncd
        stop_neighsyncd(dvs)
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        marker = dvs.add_log_marker()

        # Even number of ip4/6 neigbors updated with new mac.
        # Odd number of ipv4/6 neighbors removed
        newmacs = ["00:00:00:01:12:02", "00:00:00:01:12:03", "00:00:00:01:16:02", "00:00:00:01:16:03"]

        for i in range(len(ips)):
            if i % 2 == 0:
                dvs.runcmd("ip neigh change {} dev {} lladdr {} nud reachable".format(ips[i], intfs[i//2], newmacs[i]))
            else:
                dvs.runcmd("ip neigh del {} dev {}".format(ips[i], intfs[i//2]))

        for i in range(len(v6ips)):
            if i % 2 == 0:
                dvs.runcmd("ip -6 neigh change {} dev {} lladdr {} nud reachable".format(v6ips[i], intfs[i//2], newmacs[i]))
            else:
                dvs.runcmd("ip -6 neigh del {} dev {}".format(v6ips[i], intfs[i//2]))

        # start neighsyncd again
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)
        time.sleep(10)

        # timer is not expired yet, state should be "restored"
        swss_app_check_warmstart_state(state_db, "neighsyncd", "restored")
        time.sleep(10)

        # check neigh syncd timer is retrived from configDB
        check_neighsyncd_timer(dvs, timer_value)

        # check ipv4 and ipv6 neighbors, should see all neighbors with updated info
        for i in range(len(ips)):
            if i % 2 == 0:
                (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], ips[i]))
                assert status == True
                for v in fvs:
                    if v[0] == "neigh":
                        assert v[1] == newmacs[i]
                    if v[0] == "family":
                        assert v[1] == "IPv4"
            else:
                (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], ips[i]))
                assert status == False

        for i in range(len(v6ips)):
            if i % 2 == 0:
                (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], v6ips[i]))
                assert status == True
                for v in fvs:
                    if v[0] == "neigh":
                        assert v[1] == newmacs[i]
                    if v[0] == "family":
                        assert v[1] == "IPv6"
            else:
                (status, fvs) = tbl.get("{}:{}".format(intfs[i//2], v6ips[i]))
                assert status == False

        time.sleep(2)

        # check syslog and asic db for activities
        # 2 news, 2 deletes for ipv4 and ipv6 each
        # 4 set, 4 removes for neighbor in asic db
        check_syslog_for_neighbor_entry(dvs, marker, 2, 2, "ipv4")
        check_syslog_for_neighbor_entry(dvs, marker, 2, 2, "ipv6")
        (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
        assert nadd == 4
        assert ndel == 4

        # check restore Count
        swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")

        # post-cleanup
        dvs.runcmd("ip -s neigh flush all")
        dvs.runcmd("ip -6 -s neigh flush all")

        intf_tbl._del("{}|24.0.0.1/24".format(intfs[0]))
        intf_tbl._del("{}|28.0.0.9/24".format(intfs[1]))
        intf_tbl._del("{}|2400::1/64".format(intfs[0]))
        intf_tbl._del("{}|2800::1/64".format(intfs[1]))
        intf_tbl._del("{}".format(intfs[0]))
        intf_tbl._del("{}".format(intfs[1]))
        intf_tbl._del("{}".format(intfs[0]))
        intf_tbl._del("{}".format(intfs[1]))
        time.sleep(2)


    # TODO: The condition of warm restart readiness check is still under discussion.
    def test_OrchagentWarmRestartReadyCheck(self, dvs, testlog):

        time.sleep(1)

        dvs.warm_restart_swss("true")

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        dvs.port_admin_set("Ethernet0", "up")
        dvs.port_admin_set("Ethernet4", "up")

        dvs.servers[0].runcmd("ifconfig eth0 10.0.0.1/31")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ifconfig eth0 10.0.0.3/31")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")


        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(appl_db, swsscommon.APP_ROUTE_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])
        ps.set("2.2.2.0/24", fvs)

        fvs = swsscommon.FieldValuePairs([("nexthop","20.0.0.1"), ("ifname", "Ethernet0")])
        ps.set("3.3.3.0/24", fvs)

        time.sleep(1)
        # Should fail, since neighbor for next 20.0.0.1 has not been not resolved yet
        (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check")
        assert result == "RESTARTCHECK failed\n"

        # Should succeed, the option for skipPendingTaskCheck -s and noFreeze -n have been provided.
        # Wait up to 500 milliseconds for response from orchagent. Default wait time is 1000 milliseconds.
        (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check -n -s -w 500")
        assert result == "RESTARTCHECK succeeded\n"

        # Remove unfinished routes
        ps._del("3.3.3.0/24")

        time.sleep(1)
        (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check")
        assert result == "RESTARTCHECK succeeded\n"

        # Should fail since orchagent has been frozen at last step.
        (exitcode, result) =  dvs.runcmd("/usr/bin/orchagent_restart_check -n -s -w 500")
        assert result == "RESTARTCHECK failed\n"

        # Cleaning previously pushed route-entry to ease life of subsequent testcases.
        ps._del("2.2.2.0/24")
        time.sleep(1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        intf_tbl._del("Ethernet4|10.0.0.2/31")
        intf_tbl._del("Ethernet0")
        intf_tbl._del("Ethernet4")
        time.sleep(2)

        # recover for test cases after this one.
        dvs.stop_swss()
        dvs.start_swss()
        time.sleep(5)

    def test_swss_port_state_syncup(self, dvs, testlog):

        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        dvs.warm_restart_swss("true")

        tbl = swsscommon.Table(appl_db, swsscommon.APP_PORT_TABLE_NAME)

        restore_count = swss_get_RestoreCount(dvs, state_db)

        # update port admin state
        intf_tbl = swsscommon.Table(conf_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        intf_tbl.set("Ethernet8|10.0.0.4/31", fvs)
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        intf_tbl.set("Ethernet8", fvs)
        dvs.port_admin_set("Ethernet0", "up")
        dvs.port_admin_set("Ethernet4", "up")
        dvs.port_admin_set("Ethernet8", "up")

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

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        intf_tbl._del("Ethernet4|10.0.0.2/31")
        intf_tbl._del("Ethernet8|10.0.0.4/31")
        intf_tbl._del("Ethernet0")
        intf_tbl._del("Ethernet4")
        intf_tbl._del("Ethernet8")
        time.sleep(2)

        dvs.stop_swss()
        time.sleep(3)

        # flap the port oper status for Ethernet0, Ethernet4 and Ethernet8
        dvs.servers[0].runcmd("ip link set down dev eth0") == 0
        dvs.servers[1].runcmd("ip link set down dev eth0") == 0
        dvs.servers[2].runcmd("ip link set down dev eth0") == 0

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0
        dvs.servers[1].runcmd("ip link set up dev eth0") == 0

        time.sleep(5)
        dbobjs =[(swsscommon.APPL_DB, swsscommon.APP_PORT_TABLE_NAME + ":*"), \
            (swsscommon.STATE_DB, swsscommon.STATE_WARM_RESTART_TABLE_NAME + "|orchagent")]
        pubsubDbs = dvs.SubscribeDbObjects(dbobjs)
        dvs.start_swss()
        start_restore_neighbors(dvs)
        time.sleep(10)

        swss_check_RestoreCount(dvs, state_db, restore_count)

        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        intf_tbl.set("Ethernet8|10.0.0.4/31", fvs)
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        intf_tbl.set("Ethernet8", fvs)
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
                assert oper_status == "down"
            else:
                assert oper_status == "up"

        # check the pubsub messages.
        # No appDB port table operation should exist before orchagent state restored flag got set.
        # appDB port table status sync up happens before WARM_RESTART_TABLE reconciled flag is set
        # pubsubMessages is an ordered list of pubsub messages.
        pubsubMessages = dvs.GetSubscribedMessages(pubsubDbs)

        portOperStatusChanged = False
        # number of times that WARM_RESTART_TABLE|orchagent key was set after the first
        # appDB port table operation
        orchStateCount = 0
        for message in pubsubMessages:
            print(message)
            key = message['channel'].split(':', 1)[1]
            print(key)
            if message['data'] != 'hset' and message['data'] != 'del':
                continue
            if key.find(swsscommon.APP_PORT_TABLE_NAME)==0:
               portOperStatusChanged = True
            else:
                # found one orchagent WARM_RESTART_TABLE operation after appDB port table change
                if portOperStatusChanged == True:
                    orchStateCount += 1;

        # Only WARM_RESTART_TABLE|orchagent state=reconciled operation may exist after port oper status change.
        assert orchStateCount == 1

        #clean up arp
        dvs.runcmd("arp -d 10.0.0.1")
        dvs.runcmd("arp -d 10.0.0.3")
        dvs.runcmd("arp -d 10.0.0.5")

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        intf_tbl._del("Ethernet4|10.0.0.2/31")
        intf_tbl._del("Ethernet8|10.0.0.4/31")
        intf_tbl._del("Ethernet0")
        intf_tbl._del("Ethernet4")
        intf_tbl._del("Ethernet8")
        time.sleep(2)


    #############################################################################
    #                                                                           #
    #                        Routing Warm-Restart Testing                       #
    #                                                                           #
    #############################################################################


    ################################################################################
    #
    # Routing warm-restart testcases
    #
    ################################################################################


    def test_routing_WarmRestart(self, dvs, testlog):

        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        # Restart-timer to utilize during the following testcases
        restart_timer = 15


        #############################################################################
        #
        # Baseline configuration
        #
        #############################################################################


        # Defining create neighbor entries (4 ipv4 and 4 ip6, two each on each interface) in linux kernel
        intfs = ["Ethernet0", "Ethernet4", "Ethernet8"]

        # Enable ipv6 on docker
        dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")

        # Defining create neighbor entries (4 ipv4 and 4 ip6, two each on each interface) in linux kernel
        intf_tbl = swsscommon.Table(conf_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("{}|111.0.0.1/24".format(intfs[0]), fvs)
        intf_tbl.set("{}|1110::1/64".format(intfs[0]), fvs)
        intf_tbl.set("{}|122.0.0.1/24".format(intfs[1]), fvs)
        intf_tbl.set("{}|1220::1/64".format(intfs[1]), fvs)
        intf_tbl.set("{}|133.0.0.1/24".format(intfs[2]), fvs)
        intf_tbl.set("{}|1330::1/64".format(intfs[2]), fvs)
        intf_tbl.set("{}".format(intfs[0]), fvs)
        intf_tbl.set("{}".format(intfs[0]), fvs)
        intf_tbl.set("{}".format(intfs[1]), fvs)
        intf_tbl.set("{}".format(intfs[1]), fvs)
        intf_tbl.set("{}".format(intfs[2]), fvs)
        intf_tbl.set("{}".format(intfs[2]), fvs)
        dvs.port_admin_set(intfs[0], "up")
        dvs.port_admin_set(intfs[1], "up")
        dvs.port_admin_set(intfs[2], "up")

        time.sleep(1)

        #
        # Setting peer's ip-addresses and associated neighbor-entries
        #
        ips = ["111.0.0.2", "122.0.0.2", "133.0.0.2"]
        v6ips = ["1110::2", "1220::2", "1330::2"]
        macs = ["00:00:00:00:11:02", "00:00:00:00:12:02", "00:00:00:00:13:02"]

        for i in range(len(ips)):
            dvs.runcmd("ip neigh add {} dev {} lladdr {}".format(ips[i], intfs[i%2], macs[i]))

        for i in range(len(v6ips)):
            dvs.runcmd("ip -6 neigh add {} dev {} lladdr {}".format(v6ips[i], intfs[i%2], macs[i]))

        time.sleep(1)

        #
        # Defining baseline IPv4 non-ecmp route-entries
        #
        dvs.runcmd("ip route add 192.168.1.100/32 nexthop via 111.0.0.2")
        dvs.runcmd("ip route add 192.168.1.200/32 nexthop via 122.0.0.2")
        dvs.runcmd("ip route add 192.168.1.230/32 nexthop via 133.0.0.2")

        #
        # Defining baseline IPv4 ecmp route-entries
        #
        dvs.runcmd("ip route add 192.168.1.1/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2 nexthop via 133.0.0.2")
        dvs.runcmd("ip route add 192.168.1.2/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2 nexthop via 133.0.0.2")
        dvs.runcmd("ip route add 192.168.1.3/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2")

        #
        # Defining baseline IPv6 non-ecmp route-entries
        #
        dvs.runcmd("ip -6 route add fc00:11:11::1/128 nexthop via 1110::2")
        dvs.runcmd("ip -6 route add fc00:12:12::1/128 nexthop via 1220::2")
        dvs.runcmd("ip -6 route add fc00:13:13::1/128 nexthop via 1330::2")

        #
        # Defining baseline IPv6 ecmp route-entries
        #
        dvs.runcmd("ip -6 route add fc00:1:1::1/128 nexthop via 1110::2 nexthop via 1220::2 nexthop via 1330::2")
        dvs.runcmd("ip -6 route add fc00:2:2::1/128 nexthop via 1110::2 nexthop via 1220::2 nexthop via 1330::2")
        dvs.runcmd("ip -6 route add fc00:3:3::1/128 nexthop via 1110::2 nexthop via 1220::2")

        time.sleep(5)

        # Enabling some extra logging for troubleshooting purposes
        dvs.runcmd("swssloglevel -l INFO -c fpmsyncd")

        # Subscribe to pubsub channels for routing-state associated to swss and sairedis dbs
        pubsubAppDB  = dvs.SubscribeAppDbObject("ROUTE_TABLE")
        pubsubAsicDB = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_ROUTE_ENTRY")


        #############################################################################
        #
        # Testcase 1. Having routing-warm-reboot disabled, restart zebra and verify
        #             that the traditional/cold-boot logic is followed.
        #
        #############################################################################

        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        time.sleep(5)

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "disabled")

        # Verify that multiple changes are seen in swss and sairedis logs as there's
        # no warm-reboot logic in place.
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) != 0

        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) != 0


        #############################################################################
        #
        # Testcase 2. Restart zebra and make no control-plane changes.
        #             For this and all subsequent test-cases routing-warm-reboot
        #             feature will be kept enabled.
        #
        #############################################################################


        # Enabling bgp warmrestart and setting restart timer.
        # The following two instructions will be substituted by the commented ones
        # once the later ones are added to sonic-utilities repo.

        warm_restart_set(dvs, "bgp", "true")
        warm_restart_timer_set(dvs, "bgp", "bgp_timer", str(restart_timer))

        time.sleep(1)

        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 0

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 0


        #############################################################################
        #
        # Testcase 3. Restart zebra and add one new non-ecmp IPv4 prefix
        #
        #############################################################################

        # Stop zebra
        dvs.stop_zebra()

        # Add new prefix
        dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "192.168.100.0/24"
        assert rt_val == {"ifname": "Ethernet0", "nexthop": "111.0.0.2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "192.168.100.0/24"


        #############################################################################
        #
        # Testcase 4. Restart zebra and withdraw one non-ecmp IPv4 prefix
        #
        #############################################################################


        # Stop zebra
        dvs.stop_zebra()

        # Delete prefix
        dvs.runcmd("ip route del 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key == "192.168.100.0/24"

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key['dest'] == "192.168.100.0/24"


        #############################################################################
        #
        # Testcase 5. Restart zebra and add a new IPv4 ecmp-prefix
        #
        #############################################################################


        # Stop zebra
        dvs.stop_zebra()

        # Add prefix
        dvs.runcmd("ip route add 192.168.200.0/24 nexthop via 111.0.0.2 nexthop via 122.0.0.2 nexthop via 133.0.0.2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "192.168.200.0/24"
        assert rt_val == {"ifname": "Ethernet0,Ethernet4,Ethernet8", "nexthop": "111.0.0.2,122.0.0.2,133.0.0.2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "192.168.200.0/24"


        #############################################################################
        #
        # Testcase 6. Restart zebra and delete one existing IPv4 ecmp-prefix.
        #
        #############################################################################


        # Stop zebra
        dvs.stop_zebra()

        # Delete prefix
        dvs.runcmd("ip route del 192.168.200.0/24 nexthop via 111.0.0.2 nexthop via 122.0.0.2 nexthop via 133.0.0.2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key == "192.168.200.0/24"

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key['dest'] == "192.168.200.0/24"


        #############################################################################
        #
        # Testcase 7. Restart zebra and add one new path to an IPv4 ecmp-prefix
        #
        #############################################################################


        # Stop zebra
        dvs.stop_zebra()

        # Add new path
        dvs.runcmd("ip route del 192.168.1.3/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2")
        dvs.runcmd("ip route add 192.168.1.3/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2 nexthop via 133.0.0.2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

         # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "192.168.1.3"
        assert rt_val == {"ifname": "Ethernet0,Ethernet4,Ethernet8", "nexthop": "111.0.0.2,122.0.0.2,133.0.0.2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "192.168.1.3/32"


        #############################################################################
        #
        # Testcase 8. Restart zebra and delete one ecmp-path from an IPv4 ecmp-prefix.
        #
        #############################################################################


        # Stop zebra
        dvs.stop_zebra()

        # Delete ecmp-path
        dvs.runcmd("ip route del 192.168.1.3/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2 nexthop via 133.0.0.2")
        dvs.runcmd("ip route add 192.168.1.3/32 nexthop via 111.0.0.2 nexthop via 122.0.0.2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

         # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "192.168.1.3"
        assert rt_val == {"ifname": "Ethernet0,Ethernet4", "nexthop": "111.0.0.2,122.0.0.2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "192.168.1.3/32"


        #############################################################################
        #
        # Testcase 9. Restart zebra and add one new non-ecmp IPv6 prefix
        #
        #############################################################################


        # Stop zebra
        dvs.stop_zebra()

        # Add prefix
        dvs.runcmd("ip -6 route add fc00:4:4::1/128 nexthop via 1110::2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "fc00:4:4::1"
        assert rt_val == {"ifname": "Ethernet0", "nexthop": "1110::2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "fc00:4:4::1/128"


        #############################################################################
        #
        # Testcase 10. Restart zebra and withdraw one non-ecmp IPv6 prefix
        #
        #############################################################################

        # Stop zebra
        dvs.stop_zebra()

        # Delete prefix
        dvs.runcmd("ip -6 route del fc00:4:4::1/128 nexthop via 1110::2")
        time.sleep(1)

        # Start zebra
        dvs.start_zebra()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key == "fc00:4:4::1"

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key['dest'] == "fc00:4:4::1/128"


        #############################################################################
        #
        # Testcase 11. Restart fpmsyncd and make no control-plane changes.
        #
        #############################################################################


        # Stop fpmsyncd
        dvs.stop_fpmsyncd()

        # Start fpmsyncd
        dvs.start_fpmsyncd()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 0

        # Verify sairedis changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 0


        #############################################################################
        #
        # Testcase 12. Restart fpmsyncd and add one new non-ecmp IPv4 prefix
        #
        #############################################################################


        # Stop fpmsyncd
        dvs.stop_fpmsyncd()

        # Add new prefix
        dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)

        # Start fpmsyncd
        dvs.start_fpmsyncd()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "192.168.100.0/24"
        assert rt_val == {"ifname": "Ethernet0", "nexthop": "111.0.0.2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "192.168.100.0/24"


        #############################################################################
        #
        # Testcase 13. Restart fpmsyncd and withdraw one non-ecmp IPv4 prefix
        #
        #############################################################################


        # Stop fpmsyncd
        dvs.stop_fpmsyncd()

        # Delete prefix
        dvs.runcmd("ip route del 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)

        # Start fpmsyncd
        dvs.start_fpmsyncd()

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key == "192.168.100.0/24"

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 1
        rt_key = json.loads(delobjs[0]['key'])
        assert rt_key['dest'] == "192.168.100.0/24"


        #############################################################################
        #
        # Testcase 14. Restart zebra and add/remove a new non-ecmp IPv4 prefix. As
        #              the 'delete' instruction would arrive after the 'add' one, no
        #              changes should be pushed down to SwSS.
        #
        #############################################################################


        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        # Add/delete new prefix
        dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)
        dvs.runcmd("ip route del 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 0

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 0


        #############################################################################
        #
        # Testcase 15. Restart zebra and generate an add/remove/add for new non-ecmp
        #              IPv4 prefix. Verify that only the second 'add' instruction is
        #              honored and the corresponding update passed down to SwSS.
        #
        #############################################################################


        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        marker1 = dvs.add_log_marker("/var/log/swss/swss.rec")
        marker2 = dvs.add_log_marker("/var/log/swss/sairedis.rec")

        # Add/delete new prefix
        dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)
        dvs.runcmd("ip route del 192.168.100.0/24 nexthop via 111.0.0.2")
        time.sleep(1)
        dvs.runcmd("ip route add 192.168.100.0/24 nexthop via 122.0.0.2")
        time.sleep(1)

        # Verify FSM
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        time.sleep(restart_timer + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify the changed prefix is seen in swss
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        rt_val = json.loads(addobjs[0]['vals'])
        assert rt_key == "192.168.100.0/24"
        assert rt_val == {"ifname": "Ethernet4", "nexthop": "122.0.0.2"}

        # Verify the changed prefix is seen in sairedis
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 1 and len(delobjs) == 0
        rt_key = json.loads(addobjs[0]['key'])
        assert rt_key['dest'] == "192.168.100.0/24"


        #############################################################################
        #
        # Testcase 16. Restart zebra and make no control-plane changes.
        #             Set WARM_RESTART_TABLE|IPv4|eoiu
        #                 WARM_RESTART_TABLE|IPv6|eoiu
        #             Check route reconciliation wait time is reduced
        #             For this and all subsequent test-cases routing-warm-reboot
        #             feature will be kept enabled.
        #
        #############################################################################


        time.sleep(1)
        # Hold time from EOIU detected for both Ipv4/Ipv6 to start route reconciliation
        DEFAULT_EOIU_HOLD_INTERVAL = 3

        # change to 20 for easy timeline check
        restart_timer = 20

        # clean up as that in bgp_eoiu_marker.py
        del_entry_tbl(state_db, "BGP_STATE_TABLE", "IPv4|eoiu")
        del_entry_tbl(state_db, "BGP_STATE_TABLE", "IPv6|eoiu")

        warm_restart_timer_set(dvs, "bgp", "bgp_timer", str(restart_timer))
        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        #
        # Verify FSM:  no eoiu, just default warm restart timer
        #
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        # Periodic eoiu check timer, first wait 5 seconds, then check every 1 second
        # DEFAULT_EOIU_HOLD_INTERVAL is 3 seconds.
        # Since no EOIU set, after 3+ 5 + 1 seconds, the state still in restored state
        time.sleep(DEFAULT_EOIU_HOLD_INTERVAL + 5 +1)
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        # default restart timer kicks in:
        time.sleep(restart_timer - DEFAULT_EOIU_HOLD_INTERVAL -5)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")


        time.sleep(1)
        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        #
        # Verify FSM:  eoiu works as expected
        #
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        # Set BGP_STATE_TABLE|Ipv4|eoiu BGP_STATE_TABLE|IPv6|eoiu
        create_entry_tbl(
            state_db,
            "BGP_STATE_TABLE", "IPv4|eoiu",
            [
                ("state", "reached"),
                ("timestamp", "2019-04-25 09:39:19"),
            ]
        )
        create_entry_tbl(
            state_db,
            "BGP_STATE_TABLE", "IPv6|eoiu",
            [
                ("state", "reached"),
                ("timestamp", "2019-04-25 09:39:22"),
            ]
        )

        # after DEFAULT_EOIU_HOLD_INTERVAL + inital eoiu check timer wait time + 1 seconds: 3+5+1
        # verify that bgp reached reconciled state
        time.sleep(DEFAULT_EOIU_HOLD_INTERVAL + 5 + 1)
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 0
        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 0


        del_entry_tbl(state_db, "BGP_STATE_TABLE", "IPv4|eoiu")
        del_entry_tbl(state_db, "BGP_STATE_TABLE", "IPv6|eoiu")
        time.sleep(1)
        # Restart zebra
        dvs.stop_zebra()
        dvs.start_zebra()

        #
        # Verify FSM:  partial eoiu,  fallback to default warm restart timer
        #
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        # Set BGP_STATE_TABLE|Ipv4|eoiu but not BGP_STATE_TABLE|IPv6|eoiu
        create_entry_tbl(
            state_db,
            "BGP_STATE_TABLE", "IPv4|eoiu",
            [
                ("state", "reached"),
                ("timestamp", "2019-04-25 09:39:19"),
            ]
        )

        # Periodic eoiu check timer, first wait 5 seconds, then check every 1 second
        # DEFAULT_EOIU_HOLD_INTERVAL is 3 seconds.
        # Current bgp eoiu needs flag set on both Ipv4/Ipv6 to work, after 3+ 5 + 1 seconds, the state still in restored state
        time.sleep(DEFAULT_EOIU_HOLD_INTERVAL + 5 +1)
        swss_app_check_warmstart_state(state_db, "bgp", "restored")
        # Fall back to warm restart timer, it kicks in after 15 seconds, +1 to avoid race condition:
        time.sleep(restart_timer - DEFAULT_EOIU_HOLD_INTERVAL -5 )
        swss_app_check_warmstart_state(state_db, "bgp", "reconciled")

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAppDbObjects(pubsubAppDB)
        assert len(addobjs) == 0 and len(delobjs) == 0

        # Verify swss changes -- none are expected this time
        (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsubAsicDB)
        assert len(addobjs) == 0 and len(delobjs) == 0

        #
        # Remove route entries so they don't interfere with later tests
        #
        dvs.runcmd("ip route del 192.168.1.100/32")
        dvs.runcmd("ip route del 192.168.1.200/32")
        dvs.runcmd("ip route del 192.168.1.230/32")
        dvs.runcmd("ip route del 192.168.1.1/32")
        dvs.runcmd("ip route del 192.168.1.2/32")
        dvs.runcmd("ip route del 192.168.1.3/32")
        dvs.runcmd("ip route del 192.168.100.0/24")
        dvs.runcmd("ip -6 route del fc00:11:11::1/128")
        dvs.runcmd("ip -6 route del fc00:12:12::1/128")
        dvs.runcmd("ip -6 route del fc00:13:13::1/128")
        dvs.runcmd("ip -6 route del fc00:1:1::1/128")
        dvs.runcmd("ip -6 route del fc00:2:2::1/128")
        dvs.runcmd("ip -6 route del fc00:3:3::1/128")
        time.sleep(5)

        intf_tbl._del("{}|111.0.0.1/24".format(intfs[0]))
        intf_tbl._del("{}|1110::1/64".format(intfs[0]))
        intf_tbl._del("{}|122.0.0.1/24".format(intfs[1]))
        intf_tbl._del("{}|1220::1/64".format(intfs[1]))
        intf_tbl._del("{}|133.0.0.1/24".format(intfs[2]))
        intf_tbl._del("{}|1330::1/64".format(intfs[2]))
        intf_tbl._del("{}".format(intfs[0]))
        intf_tbl._del("{}".format(intfs[0]))
        intf_tbl._del("{}".format(intfs[1]))
        intf_tbl._del("{}".format(intfs[1]))
        intf_tbl._del("{}".format(intfs[2]))
        intf_tbl._del("{}".format(intfs[2]))
        time.sleep(2)

    @pytest.mark.xfail(reason="Test unstable, blocking PR builds")
    def test_system_warmreboot_neighbor_syncup(self, dvs, testlog):

        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        #enable ipv6 on docker
        dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")

        # flush all neighs first
        flush_neigh_entries(dvs)
        time.sleep(5)

        warm_restart_set(dvs, "system", "true")

        # Test neighbors on NUM_INTF (e,g 8) interfaces
        # Ethernet32/36/.../60, with ip: 32.0.0.1/24... 60.0.0.1/24
        # ipv6: 3200::1/64...6000::1/64
        # bring up the servers'interfaces and assign NUM_NEIGH_PER_INTF (e,g 128) ips per interface
        macs = []
        intf_tbl = swsscommon.Table(conf_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        for i in range(8, 8+NUM_INTF):
            # set timeout to be the same as real HW
            # set stale timer bigger to avoid testbed difference related timing issues.
            # set ip on server facing interfaces
            # bring servers' interface up, save the macs
            dvs.runcmd("sysctl -w net.ipv4.neigh.Ethernet{}.base_reachable_time_ms=1800000".format(i*4))
            dvs.runcmd("sysctl -w net.ipv6.neigh.Ethernet{}.base_reachable_time_ms=1800000".format(i*4))
            dvs.runcmd("sysctl -w net.ipv4.neigh.Ethernet{}.gc_stale_time=600".format(i*4))
            dvs.runcmd("sysctl -w net.ipv6.neigh.Ethernet{}.gc_stale_time=600".format(i*4))
            dvs.runcmd("ip addr flush dev Ethernet{}".format(i*4))
            intf_tbl.set("Ethernet{}|{}.0.0.1/24".format(i*4, i*4), fvs)
            intf_tbl.set("Ethernet{}|{}00::1/64".format(i*4, i*4), fvs)
            intf_tbl.set("Ethernet{}".format(i*4, i*4), fvs)
            intf_tbl.set("Ethernet{}".format(i*4, i*4), fvs)
            dvs.port_admin_set("Ethernet{}".format(i*4), "up")
            dvs.servers[i].runcmd("ip link set up dev eth0")
            dvs.servers[i].runcmd("ip addr flush dev eth0")
            #result = dvs.servers[i].runcmd_output("ifconfig eth0 | grep HWaddr | awk '{print $NF}'")
            result = dvs.servers[i].runcmd_output("cat /sys/class/net/eth0/address")
            macs.append(result.strip())

        #
        # Testcase 1:
        # Setup initial neigbors
        setup_initial_neighbors(dvs)

        # Check the neighbor entries are inserted correctly
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "NEIGH_TABLE")

        # number of neighbors should match what we configured
        # ipv4/ipv6 entries and loopback
        check_redis_neigh_entries(dvs, tbl, 2*NUM_OF_NEIGHS)

        # All neighbor entries should match
        for i in range(8, 8+NUM_INTF):
            for j in range(NUM_NEIGH_PER_INTF):
                (status, fvs) = tbl.get("Ethernet{}:{}.0.0.{}".format(i*4, i*4, j+2))
                assert status == True
                for v in fvs:
                    if v[0] == "family":
                        assert v[1] == "IPv4"
                    if v[0] == "neigh":
                        assert v[1] == macs[i-8]

                (status, fvs) = tbl.get("Ethernet{}:{}00::{}".format(i*4, i*4, j+2))
                assert status == True
                for v in fvs:
                    if v[0] == "family":
                        assert v[1] == "IPv6"
                    if v[0] == "neigh":
                        assert v[1] == macs[i-8]

        #
        # Testcase 2:
        # Stop neighsyncd, appDB entries should be reserved
        # flush kernel neigh table to simulate warm reboot
        # start neighsyncd, start restore_neighbors service to restore the neighbor table in kernel
        # check all neighbors learned in kernel
        # no changes should be there in syslog and sairedis.rec

        # get restore_count
        restore_count = swss_get_RestoreCount(dvs, state_db)

        # stop neighsyncd and sairedis.rec
        stop_neighsyncd(dvs)
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        time.sleep(3)
        flush_neigh_entries(dvs)
        time.sleep(3)

        # check neighbors are gone
        check_kernel_reachable_neigh_num(dvs, 0)

        # start neighsyncd and restore_neighbors
        marker = dvs.add_log_marker()
        pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)

        # should finish the store within 10 seconds
        time.sleep(10)

        check_kernel_reachable_v4_neigh_num(dvs, NUM_OF_NEIGHS)
        check_kernel_reachable_v6_neigh_num(dvs, NUM_OF_NEIGHS)

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
        # Stop neighsyncd, appDB entries should be reserved
        # flush kernel neigh table to simulate warm reboot
        # Remove half of ips of servers' interfaces, add new half of ips
        # start neighsyncd, start restore_neighbors service to restore the neighbor table in kernel
        # check all new neighbors learned in kernel
        # no changes should be there in syslog and sairedis.rec

        # get restore_count
        restore_count = swss_get_RestoreCount(dvs, state_db)

        # stop neighsyncd and sairedis.rec
        stop_neighsyncd(dvs)
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        time.sleep(3)

        del_and_add_neighbors(dvs)

        flush_neigh_entries(dvs)
        time.sleep(3)

        # check neighbors are gone
        check_kernel_reachable_neigh_num(dvs, 0)

        # start neighsyncd and restore_neighbors
        marker = dvs.add_log_marker()
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)

        # should finish the store within 10 seconds
        time.sleep(10)

        check_kernel_reachable_v4_neigh_num(dvs, NUM_OF_NEIGHS//2)
        check_kernel_reachable_v6_neigh_num(dvs, NUM_OF_NEIGHS//2)

        check_kernel_stale_v4_neigh_num(dvs, NUM_OF_NEIGHS//2)
        check_kernel_stale_v6_neigh_num(dvs, NUM_OF_NEIGHS//2)

        # check syslog and sairedis.rec file for activities
        check_syslog_for_neighbor_entry(dvs, marker, 0, 0, "ipv4")
        check_syslog_for_neighbor_entry(dvs, marker, 0, 0, "ipv6")
        (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
        assert nadd == 0
        assert ndel == 0

        # check restore Count
        swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")

        # Test case 4:
        # ping the new ips, should get it into appDB
        marker = dvs.add_log_marker()

        ping_new_ips(dvs)

        check_kernel_reachable_v4_neigh_num(dvs, NUM_OF_NEIGHS)
        check_kernel_reachable_v6_neigh_num(dvs, NUM_OF_NEIGHS)

        check_redis_neigh_entries(dvs, tbl, 2*(NUM_OF_NEIGHS+NUM_OF_NEIGHS//2))

        (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
        assert nadd == NUM_OF_NEIGHS #ipv4 and ipv6
        assert ndel == 0

        # Remove stale entries manually
        for i in range(8, 8+NUM_INTF):
            for j in range(NUM_NEIGH_PER_INTF//2):
                dvs.runcmd(['sh', '-c', "ip neigh del {}.0.0.{} dev Ethernet{}".format(i*4, j+NUM_NEIGH_PER_INTF//2+2, i*4)])
                dvs.runcmd(['sh', '-c', "ip -6 neigh del {}00::{} dev Ethernet{}".format(i*4, j+NUM_NEIGH_PER_INTF//2+2, i*4)])

        time.sleep(5)

        check_kernel_reachable_v4_neigh_num(dvs, NUM_OF_NEIGHS)
        check_kernel_reachable_v6_neigh_num(dvs, NUM_OF_NEIGHS)

        check_kernel_stale_v4_neigh_num(dvs, 0)
        check_kernel_stale_v6_neigh_num(dvs, 0)

        check_redis_neigh_entries(dvs, tbl, 2*NUM_OF_NEIGHS)

        (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
        assert nadd == 0
        assert ndel == NUM_OF_NEIGHS #ipv4 and ipv6

        #
        # Testcase 5:
        # Stop neighsyncd, appDB entries should be reserved
        # flush kernel neigh table to simulate warm reboot
        # keep half of the interface down
        # start neighsyncd, start restore_neighbors service to restore the neighbor table in kernel
        # check all new neighbors with interface up to be learned in kernel
        # syslog/sai log should show half of the entries stale/deleted

        # get restore_count
        restore_count = swss_get_RestoreCount(dvs, state_db)

        # stop neighsyncd and sairedis.rec
        stop_neighsyncd(dvs)
        del_entry_tbl(state_db, "NEIGH_RESTORE_TABLE", "Flags")
        time.sleep(3)

        flush_neigh_entries(dvs)
        time.sleep(3)

        # check neighbors are gone
        check_kernel_reachable_neigh_num(dvs, 0)

        # bring down half of the links
        for i in range(8, 8+NUM_INTF//2):
            dvs.runcmd("ip link set down dev Ethernet{}".format(i*4))

        # start neighsyncd and restore_neighbors
        start_neighsyncd(dvs)
        start_restore_neighbors(dvs)

        # restore for up interfaces should be done within 10 seconds
        time.sleep(10)

        check_kernel_reachable_v4_neigh_num(dvs, NUM_OF_NEIGHS//2)
        check_kernel_reachable_v6_neigh_num(dvs, NUM_OF_NEIGHS//2)

        restoretbl = swsscommon.Table(state_db, swsscommon.STATE_NEIGH_RESTORE_TABLE_NAME)

        # waited 10 above already
        i = 10
        while (not kernel_restore_neighs_done(restoretbl)):
            print("Waiting for kernel neighbors restore process done: {} seconds".format(i))
            time.sleep(10)
            i += 10

        time.sleep(10)


        # check syslog and sairedis.rec file for activities
        #check_syslog_for_neighbor_entry(dvs, marker, 0, NUM_OF_NEIGHS//2, "ipv4")
        #check_syslog_for_neighbor_entry(dvs, marker, 0, NUM_OF_NEIGHS//2, "ipv6")
        (nadd, ndel) = dvs.CountSubscribedObjects(pubsub)
        assert nadd == 0
        assert ndel == NUM_OF_NEIGHS

        # check restore Count
        swss_app_check_RestoreCount_single(state_db, restore_count, "neighsyncd")

        # disable system warm restart
        warm_restart_set(dvs, "system", "false")

        for i in range(8, 8+NUM_INTF):
            intf_tbl._del("Ethernet{}|{}.0.0.1/24".format(i*4, i*4))
            intf_tbl._del("Ethernet{}|{}00::1/64".format(i*4, i*4))
            intf_tbl._del("Ethernet{}".format(i*4, i*4))
            intf_tbl._del("Ethernet{}".format(i*4, i*4))

    def test_VrfMgrdWarmRestart(self, dvs, testlog):

        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        dvs.warm_restart_swss("true")

        # bring up interface
        dvs.port_admin_set("Ethernet0", "up")
        dvs.port_admin_set("Ethernet4", "up")

        # create vrf
        create_entry_tbl(conf_db, "VRF", "Vrf_1", [('empty', 'empty')])
        create_entry_tbl(conf_db, "VRF", "Vrf_2", [('empty', 'empty')])

        intf_tbl = swsscommon.Table(conf_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("vrf_name", "Vrf_1")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        intf_tbl.set("Ethernet0|12.0.0.1/24", fvs)
        intf_tbl.set("Ethernet4|13.0.0.1/24", fvs)

        time.sleep(1)

        dvs.servers[0].runcmd("ifconfig eth0 12.0.0.2/24")
        dvs.servers[0].runcmd("ip route add default via 12.0.0.1")

        dvs.servers[1].runcmd("ifconfig eth0 13.0.0.2/24")
        dvs.servers[1].runcmd("ip route add default via 13.0.0.1")

        time.sleep(1)

        # Ping should work between servers via vs port interfaces
        ping_stats = dvs.servers[0].runcmd("ping -c 1 13.0.0.2")
        assert ping_stats == 0
        time.sleep(1)

        tbl = swsscommon.Table(appl_db, "NEIGH_TABLE")
        (status, fvs) = tbl.get("Ethernet0:12.0.0.2")
        assert status == True

        (status, fvs) = tbl.get("Ethernet4:13.0.0.2")
        assert status == True

        (exitcode, vrf_before) = dvs.runcmd(['sh', '-c', "ip link show | grep Vrf"])

        dvs.runcmd(['sh', '-c', 'pkill -x vrfmgrd'])

        pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE")

        dvs.runcmd(['sh', '-c', 'supervisorctl start vrfmgrd'])
        time.sleep(2)

        # kernel vrf config should be kept the same
        (exitcode, vrf_after) = dvs.runcmd(['sh', '-c', "ip link show | grep Vrf"])
        assert vrf_after == vrf_before

        # VIRTUAL_ROUTER/ROUTE_ENTRY/NEIGH_ENTRY should be kept the same
        (nadd, ndel) = dvs.CountSubscribedObjects(pubsub, ignore=["SAI_OBJECT_TYPE_FDB_ENTRY"])
        assert nadd == 0
        assert ndel == 0

        # new ip on server 1
        dvs.servers[1].runcmd("ifconfig eth0 13.0.0.3/24")
        dvs.servers[1].runcmd("ip route add default via 13.0.0.1")

        # Ping should work between servers via vs port interfaces
        ping_stats = dvs.servers[0].runcmd("ping -c 1 13.0.0.3")
        assert ping_stats == 0

        # new neighbor learn on vs
        (status, fvs) = tbl.get("Ethernet4:13.0.0.3")
        assert status == True

        # flush all neigh entries
        dvs.runcmd("ip link set group default arp off")
        dvs.runcmd("ip link set group default arp on")

        # remove interface Ethernet4 from vrf_1, add it to vrf_2
        intf_tbl._del("Ethernet4|13.0.0.1/24")
        intf_tbl._del("Ethernet4")
        time.sleep(1)

        intf_tbl = swsscommon.Table(conf_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("vrf_name", "Vrf_2")])
        intf_tbl.set("Ethernet4", fvs)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        intf_tbl.set("Ethernet4|13.0.0.1/24", fvs)
        time.sleep(1)

        # Ping should not work
        ping_stats = dvs.servers[0].runcmd("ping -c 1 13.0.0.3")
        assert ping_stats != 0

        # remove interface Ethernet0 from vrf_1, add it to vrf_2
        intf_tbl._del("Ethernet0|12.0.0.1/24")
        intf_tbl._del("Ethernet0")
        time.sleep(1)
        fvs = swsscommon.FieldValuePairs([("vrf_name", "Vrf_2")])
        intf_tbl.set("Ethernet0", fvs)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        intf_tbl.set("Ethernet0|12.0.0.1/24", fvs)
        time.sleep(1)

        # Ping should work between servers via vs port interfaces
        ping_stats = dvs.servers[0].runcmd("ping -c 1 13.0.0.3")
        assert ping_stats == 0

        (status, fvs) = tbl.get("Ethernet4:13.0.0.3")
        assert status == True

        intf_tbl._del("Ethernet0|12.0.0.1/24")
        intf_tbl._del("Ethernet4|13.0.0.1/24")
        intf_tbl._del("Ethernet0")
        intf_tbl._del("Ethernet4")
        del_entry_tbl(conf_db, "VRF", "Vrf_1")
        del_entry_tbl(conf_db, "VRF", "Vrf_2")
        dvs.servers[0].runcmd("ifconfig eth0 0")
        dvs.servers[1].runcmd("ifconfig eth0 0")
        time.sleep(2)

    @pytest.fixture(scope="class")
    def setup_erspan_neighbors(self, dvs):
        dvs.setup_db()

        dvs.set_interface_status("Ethernet12", "up")
        dvs.set_interface_status("Ethernet16", "up")
        dvs.set_interface_status("Ethernet20", "up")

        dvs.add_ip_address("Ethernet12", "10.0.0.0/31")
        dvs.add_ip_address("Ethernet16", "11.0.0.0/31")
        dvs.add_ip_address("Ethernet20", "12.0.0.0/31")

        dvs.add_neighbor("Ethernet12", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_neighbor("Ethernet16", "11.0.0.1", "03:04:06:08:10:12")
        dvs.add_neighbor("Ethernet20", "12.0.0.1", "04:04:06:08:10:12")

        dvs.add_route("2.2.2.2", "10.0.0.1")

        yield

        dvs.remove_route("2.2.2.2")

        dvs.remove_neighbor("Ethernet12", "10.0.0.1")
        dvs.remove_neighbor("Ethernet16", "11.0.0.1")
        dvs.remove_neighbor("Ethernet20", "12.0.0.1")

        dvs.remove_ip_address("Ethernet12", "10.0.0.0/31")
        dvs.remove_ip_address("Ethernet16", "11.0.0.0/31")
        dvs.remove_ip_address("Ethernet20", "12.0.0.1/31")

        dvs.set_interface_status("Ethernet12", "down")
        dvs.set_interface_status("Ethernet16", "down")
        dvs.set_interface_status("Ethernet20", "down")

    @pytest.mark.usefixtures("dvs_mirror_manager", "setup_erspan_neighbors")
    def test_MirrorSessionWarmReboot(self, dvs):
        dvs.setup_db()

        # Setup the mirror session
        self.dvs_mirror.create_erspan_session("test_session", "1.1.1.1", "2.2.2.2", "0x6558", "8", "100", "0")

        # Verify the monitor port
        state_db = dvs.get_state_db()
        state_db.wait_for_field_match("MIRROR_SESSION_TABLE", "test_session", {"monitor_port": "Ethernet12"})

        # Setup ECMP routes to the session destination
        dvs.change_route_ecmp("2.2.2.2", ["12.0.0.1", "11.0.0.1", "10.0.0.1"])

        # Monitor port should not change b/c routes are ECMP
        state_db.wait_for_field_match("MIRROR_SESSION_TABLE", "test_session", {"monitor_port": "Ethernet12"})

        dvs.warm_restart_swss("true")
        dvs.stop_swss()
        dvs.start_swss()

        dvs.check_swss_ready()

        # Monitor port should not change b/c destination is frozen
        state_db.wait_for_field_match("MIRROR_SESSION_TABLE", "test_session", {"monitor_port": "Ethernet12"})

        self.dvs_mirror.remove_mirror_session("test_session")

        # Revert the route back to the fixture-defined route
        dvs.change_route("2.2.2.2", "10.0.0.1")

        # Reset for test cases after this one
        dvs.stop_swss()
        dvs.start_swss()
        dvs.check_swss_ready()

    @pytest.mark.usefixtures("dvs_mirror_manager", "dvs_policer_manager", "setup_erspan_neighbors")
    def test_EverflowWarmReboot(self, dvs, dvs_acl):
        # Setup the policer
        self.dvs_policer.create_policer("test_policer")
        self.dvs_policer.verify_policer("test_policer")

        # Setup the mirror session
        self.dvs_mirror.create_erspan_session("test_session", "1.1.1.1", "2.2.2.2", "0x6558", "8", "100", "0", policer="test_policer")

        state_db = dvs.get_state_db()
        state_db.wait_for_field_match("MIRROR_SESSION_TABLE", "test_session", {"status": "active"})

        # Create the mirror table
        dvs_acl.create_acl_table("EVERFLOW_TEST", "MIRROR", ["Ethernet12"])


        # TODO: The standard methods for counting ACL tables and ACL rules break down after warm reboot b/c the OIDs
        # of the default tables change. We also can't just re-initialize the default value b/c we've added another
        # table and rule that aren't part of the base device config. We should come up with a way to handle warm reboot
        # changes more gracefully to make it easier for future tests.
        asic_db = dvs.get_asic_db()
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", 1 + len(asic_db.default_acl_tables))

        # Create a mirror rule
        dvs_acl.create_mirror_acl_rule("EVERFLOW_TEST", "TEST_RULE", {"SRC_IP": "20.0.0.2"}, "test_session")
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1 + len(asic_db.default_acl_entries))

        # Execute the warm reboot
        dvs.warm_restart_swss("true")
        dvs.stop_swss()
        dvs.start_swss()

        # Make sure the system is stable
        dvs.check_swss_ready()

        # Verify that the ASIC DB is intact
        self.dvs_policer.verify_policer("test_policer")
        state_db.wait_for_field_match("MIRROR_SESSION_TABLE", "test_session", {"status": "active"})

        asic_db = dvs.get_asic_db()
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", 1 + len(asic_db.default_acl_tables))
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1 + len(asic_db.default_acl_entries))

        # Clean up
        dvs_acl.remove_acl_rule("EVERFLOW_TEST", "TEST_RULE")
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", len(asic_db.default_acl_entries))

        dvs_acl.remove_acl_table("EVERFLOW_TEST")
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", len(asic_db.default_acl_tables))

        self.dvs_mirror.remove_mirror_session("test_session")
        self.dvs_mirror.verify_no_mirror()

        self.dvs_policer.remove_policer("test_policer")
        self.dvs_policer.verify_no_policer()

        # Reset for test cases after this one
        dvs.stop_swss()
        dvs.start_swss()
        dvs.check_swss_ready()

    def test_TunnelMgrdWarmRestart(self, dvs):
        tunnel_name = "MuxTunnel0"
        tunnel_table = "TUNNEL_DECAP_TABLE"
        tunnel_params = {
            "tunnel_type": "IPINIP",
            "dst_ip": "10.1.0.32",
            "dscp_mode": "uniform",
            "ecn_mode": "standard",
            "ttl_mode": "pipe"
        }
        
        pubsub = dvs.SubscribeAppDbObject(tunnel_table)

        dvs.runcmd("config warm_restart enable swss")
        config_db = dvs.get_config_db()
        config_db.create_entry("TUNNEL", tunnel_name, tunnel_params)

        app_db = dvs.get_app_db()
        app_db.wait_for_matching_keys(tunnel_table, [tunnel_name])

        nadd, ndel = dvs.CountSubscribedObjects(pubsub)
        assert nadd == len(tunnel_params)
        assert ndel == 1  # Expect 1 deletion as part of table creation

        dvs.runcmd("supervisorctl restart tunnelmgrd")
        dvs.check_services_ready()
        nadd, ndel = dvs.CountSubscribedObjects(pubsub)
        assert nadd == 0
        assert ndel == 0

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
