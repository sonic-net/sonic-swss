import os
import re
import time
import json
import pytest

from swsscommon import swsscommon

def getCrmCounterValue(dvs, key, counter):

    counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    crm_stats_table = swsscommon.Table(counters_db, 'CRM')

    for k in crm_stats_table.get(key)[1]:
        if k[0] == counter:
            return int(k[1])

    return 0

def check_syslog(dvs, marker, err_log, expected_cnt):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, err_log)])
    assert num.strip() >= str(expected_cnt)

def crm_update(dvs, field, value):
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(cfg_db, "CRM")
    fvs = swsscommon.FieldValuePairs([(field, value)])
    tbl.set("Config", fvs)
    time.sleep(1)

class TestCrm(object):
    def test_CrmFdbEntry(self, dvs, testlog):

        # disable ipv6 on Ethernet8 neighbor as once ipv6 link-local address is
        # configured, server 2 will send packet which can switch to learn another
        # mac and fail the test.
        dvs.servers[2].runcmd("sysctl -w net.ipv6.conf.eth0.disable_ipv6=1")
        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '1000')

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_available')

        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        # create a FDB entry
        tbl = swsscommon.ProducerStateTable(app_db, "FDB_TABLE")
        fvs = swsscommon.FieldValuePairs([("port","Ethernet8"),("type","dynamic")])
        tbl.set("Vlan2:52-54-00-25-06-E9", fvs)

        # create vlan
        tbl = swsscommon.Table(cfg_db, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", "2")])
        tbl.set("Vlan2", fvs)

        # create vlan member
        tbl = swsscommon.Table(cfg_db, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan2|Ethernet8", fvs)

        # update available counter
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # update available counter
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_available')

        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "fdb_entry_high_threshold", "90")
        crm_update(dvs, "fdb_entry_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "FDB_ENTRY THRESHOLD_EXCEEDED for TH_FREE", 1)

        # enable ipv6 on server 2
        dvs.servers[2].runcmd("sysctl -w net.ipv6.conf.eth0.disable_ipv6=0")

    def test_CrmIpv4Route(self, dvs, testlog):

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        dvs.port_admin_set("Ethernet0", "up")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '1000')

        # add static neighbor
        dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_available')

        # add route and update available counter
        ps.set("2.2.2.0/24", fvs)
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove route and update available counter
        ps._del("2.2.2.0/24")
        dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "ipv4_route_high_threshold", "90")
        crm_update(dvs, "ipv4_route_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV4_ROUTE THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        time.sleep(2)

    def test_CrmIpv6Route(self, dvs, testlog):

        # Enable IPv6 routing
        dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")
        time.sleep(2)

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet0|fc00::1/126", fvs)
        dvs.port_admin_set("Ethernet0", "up")

        dvs.servers[0].runcmd("ifconfig eth0 inet6 add fc00::2/126")
        dvs.servers[0].runcmd("ip -6 route add default via fc00::1")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY', '1000')

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping6 -c 4 fc00::1")

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","fc00::2"), ("ifname", "Ethernet0")])

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_available')

        # add route and update available counter
        ps.set("2001::/64", fvs)
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove route and update available counter
        ps._del("2001::/64")
        dvs.runcmd("ip -6 neigh del fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "ipv6_route_high_threshold", "90")
        crm_update(dvs, "ipv6_route_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV6_ROUTE THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|fc00::1/126")
        time.sleep(2)

    def test_CrmIpv4Nexthop(self, dvs, testlog):

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet0", fvs)
        dvs.port_admin_set("Ethernet0", "up")
        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY', '1000')

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_available')

        # add nexthop and update available counter
        dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove nexthop and update available counter
        dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "ipv4_nexthop_high_threshold", "90")
        crm_update(dvs, "ipv4_nexthop_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV4_NEXTHOP THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        time.sleep(2)

    def test_CrmIpv6Nexthop(self, dvs, testlog):

        # Enable IPv6 routing
        dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")
        time.sleep(2)

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet0|fc00::1/126", fvs)
        dvs.port_admin_set("Ethernet0", "up")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY', '1000')

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_available')

        # add nexthop and update available counter
        dvs.runcmd("ip -6 neigh replace fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove nexthop and update available counter
        dvs.runcmd("ip -6 neigh del fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "ipv6_nexthop_high_threshold", "90")
        crm_update(dvs, "ipv6_nexthop_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV6_NEXTHOP THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|fc00::1/126")
        time.sleep(2)

    def test_CrmIpv4Neighbor(self, dvs, testlog):

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        dvs.port_admin_set("Ethernet0", "up")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY', '1000')

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_available')

        # add neighbor and update available counter
        dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove neighbor and update available counter
        dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "ipv4_neighbor_high_threshold", "90")
        crm_update(dvs, "ipv4_neighbor_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV4_NEIGHBOR THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        time.sleep(2)

    def test_CrmIpv6Neighbor(self, dvs, testlog):

        # Enable IPv6 routing
        dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")
        time.sleep(2)

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet0|fc00::1/126", fvs)
        dvs.port_admin_set("Ethernet0", "up")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY', '1000')

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_available')

        # add neighbor and update available counter
        dvs.runcmd("ip -6 neigh replace fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove neighbor and update available counter
        dvs.runcmd("ip -6 neigh del fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "ipv6_neighbor_high_threshold", "90")
        crm_update(dvs, "ipv6_neighbor_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV6_NEIGHBOR THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|fc00::1/126")
        time.sleep(2)

    def test_CrmNexthopGroup(self, dvs, testlog):

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        dvs.port_admin_set("Ethernet0", "up")
        dvs.port_admin_set("Ethernet4", "up")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY', '1000')

        # add neighbors
        dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.runcmd("ip neigh replace 10.0.0.3 lladdr 11:22:33:44:55:66 dev Ethernet4")

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3"), ("ifname", "Ethernet0,Ethernet4")])

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_available')

        # add route and update available counter
        ps.set("2.2.2.0/24", fvs)
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY', '999')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_available')

        assert new_used_counter - used_counter == 1
        assert avail_counter - new_avail_counter == 1

        # remove route and update available counter
        ps._del("2.2.2.0/24")
        dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.runcmd("ip neigh del 10.0.0.3 lladdr 11:22:33:44:55:66 dev Ethernet4")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "nexthop_group_high_threshold", "90")
        crm_update(dvs, "nexthop_group_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "NEXTHOP_GROUP THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        intf_tbl._del("Ethernet4|10.0.0.2/31")
        time.sleep(2)

    def test_CrmNexthopGroupMember(self, dvs, testlog):

        # down, then up to generate port up signal
        dvs.servers[0].runcmd("ip link set down dev eth0") == 0
        dvs.servers[1].runcmd("ip link set down dev eth0") == 0
        dvs.servers[0].runcmd("ip link set up dev eth0") == 0
        dvs.servers[1].runcmd("ip link set up dev eth0") == 0

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        dvs.port_admin_set("Ethernet0", "up")
        dvs.port_admin_set("Ethernet4", "up")

        crm_update(dvs, "polling_interval", "1")

        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY', '1000')

        # add neighbors
        dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.runcmd("ip neigh replace 10.0.0.3 lladdr 11:22:33:44:55:66 dev Ethernet4")

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3"), ("ifname", "Ethernet0,Ethernet4")])

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_available')

        # add route and update available counter
        ps.set("2.2.2.0/24", fvs)
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY', '998')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_available')

        assert new_used_counter - used_counter == 2
        assert avail_counter - new_avail_counter == 2

        # remove route and update available counter
        ps._del("2.2.2.0/24")
        dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
        dvs.runcmd("ip neigh del 10.0.0.3 lladdr 11:22:33:44:55:66 dev Ethernet4")
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY', '1000')

        time.sleep(2)

        # get counters
        new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_used')
        new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_available')

        assert new_used_counter == used_counter
        assert new_avail_counter == avail_counter

        marker = dvs.add_log_marker()
        crm_update(dvs, "polling_interval", "2")
        crm_update(dvs, "nexthop_group_member_high_threshold", "90")
        crm_update(dvs, "nexthop_group_member_threshold_type", "free")
        time.sleep(2)
        check_syslog(dvs, marker, "NEXTHOP_GROUP_MEMBER THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        intf_tbl._del("Ethernet4|10.0.0.2/31")
        time.sleep(2)

    def test_CrmAcl(self, dvs, testlog):

        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        crm_update(dvs, "polling_interval", "1")
        time.sleep(1)

        bind_ports = ["Ethernet0", "Ethernet4"]

        old_table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_table_used')

        value = {
            "count": 1,
            "list": [
                {
                    "stage": "SAI_ACL_STAGE_INGRESS",
                    "bind_point": "SAI_ACL_BIND_POINT_TYPE_PORT",
                    "avail_num": "4294967295"
                }
            ]
        }
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE', json.dumps(value))
        time.sleep(2)

        marker = dvs.add_log_marker()
        # create ACL table
        ttbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
        ttbl.set("test", fvs)
        time.sleep(2)
        check_syslog(dvs, marker, "ACL_TABLE Exception occurred (div by Zero)", 1)

        # create ACL rule
        rtbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"), ("PACKET_ACTION", "FORWARD"), ("L4_SRC_PORT", "65000")])
        rtbl.set("test|acl_test_rule", fvs)

        time.sleep(2)

        new_table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_table_used')
        table_used_counter = new_table_used_counter - old_table_used_counter
        assert table_used_counter == 1

        # get ACL table key
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        acl_tables = [k for k in atbl.getKeys() if k not in dvs.asicdb.default_acl_tables]
        key = "ACL_TABLE_STATS:{0}".format(acl_tables[0].replace('oid:', ''))

        entry_used_counter = getCrmCounterValue(dvs, key, 'crm_stats_acl_entry_used')
        assert entry_used_counter == 1

        cnt_used_counter = getCrmCounterValue(dvs, key, 'crm_stats_acl_counter_used')
        assert entry_used_counter == 1

        # remove ACL rule
        rtbl._del("test|acl_test_rule")

        time.sleep(2)

        entry_used_counter = getCrmCounterValue(dvs, key, 'crm_stats_acl_entry_used')
        assert entry_used_counter == 0

        cnt_used_counter = getCrmCounterValue(dvs, key, 'crm_stats_acl_counter_used')
        assert cnt_used_counter == 0

        # remove ACL table
        ttbl._del("test")

        time.sleep(2)

        new_table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_table_used')
        table_used_counter = new_table_used_counter - old_table_used_counter
        assert table_used_counter == 0

        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        crm_stats_table = swsscommon.Table(counters_db, 'CRM')
        keys = crm_stats_table.getKeys()
        assert key not in keys

    def test_CrmAclGroup(self, dvs, testlog):

        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        crm_update(dvs, "polling_interval", "1")
        bind_ports = ["Ethernet0", "Ethernet4", "Ethernet8"]

        # create ACL table
        tbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "testv6"), ("type", "L3V6"), ("ports", ",".join(bind_ports))])
        tbl.set("test-aclv6", fvs)

        time.sleep(2)
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        entry_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_group_used')
        assert entry_used_counter == 3

        # remove ACL table
        #tbl._del("test-aclv6")
        #time.sleep(2)
        #atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        #table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_group_used')
        #assert table_used_counter == 0

    def test_CrmSnatEntry(self, dvs, testlog):

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_snat_entry_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_snat_entry_available')
        assert used_counter == 0
        assert avail_counter != 0

    def test_CrmDnatEntry(self, dvs, testlog):

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_dnat_entry_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_dnat_entry_available')
        assert used_counter == 0
        assert avail_counter != 0

    def test_CrmResetThresholdExceedCount(self, dvs, testlog):

        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        dvs.port_admin_set("Ethernet0", "up")

        crm_update(dvs, "polling_interval", "1")

        # add static neighbor
        dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

        # add route to make sure used count at least 1 and update available counter
        ps.set("2.2.2.0/24", fvs)
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '1000')

        time.sleep(2)

        # get counters
        used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_used')
        avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_available')

        crm_update(dvs, "ipv4_route_low_threshold", "0")

        # for changing to free type will rest crm threshold exceed count, previous type can not be same free type
        crm_update(dvs, "ipv4_route_threshold_type", "percentage")

        # trigger exceed high threshold of free type
        marker = dvs.add_log_marker()
        crm_update(dvs, "ipv4_route_high_threshold", str(avail_counter))
        crm_update(dvs, "ipv4_route_threshold_type", "free")
        time.sleep(20)
        check_syslog(dvs, marker, "IPV4_ROUTE THRESHOLD_EXCEEDED for TH_FREE", 1)

        # crm threshold exceed count will be reset when threshold type changed
        marker = dvs.add_log_marker()
        crm_update(dvs, "ipv4_route_high_threshold", str(used_counter))
        crm_update(dvs, "ipv4_route_threshold_type", "used")
        time.sleep(2)
        check_syslog(dvs, marker, "IPV4_ROUTE THRESHOLD_EXCEEDED for TH_USED", 1)

        # remove route
        ps._del("2.2.2.0/24")
        dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        time.sleep(2)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
