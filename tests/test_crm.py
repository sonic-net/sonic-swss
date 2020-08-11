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

def getCrmConfigValue(dvs, key, counter):

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    crm_stats_table = swsscommon.Table(config_db, 'CRM')

    for k in crm_stats_table.get(key)[1]:
        if k[0] == counter:
            return int(k[1])

def getCrmConfigStr(dvs, key, counter):

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    crm_stats_table = swsscommon.Table(config_db, 'CRM')

    for k in crm_stats_table.get(key)[1]:
        if k[0] == counter:
            return k[1]
    return ""

def check_syslog(dvs, marker, err_log, expected_cnt):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, err_log)])
    assert num.strip() >= str(expected_cnt)


class TestCrm(object):
    def test_CrmFdbEntry(self, dvs, testlog):

        # disable ipv6 on Ethernet8 neighbor as once ipv6 link-local address is
        # configured, server 2 will send packet which can switch to learn another
        # mac and fail the test.
        dvs.servers[2].runcmd("sysctl -w net.ipv6.conf.eth0.disable_ipv6=1")
        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds fdb high 90")
        dvs.runcmd("crm config thresholds fdb type free")
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
        dvs.runcmd("config interface startup Ethernet0")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds ipv4 route high 90")
        dvs.runcmd("crm config thresholds ipv4 route type free")
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
        dvs.runcmd("config interface startup Ethernet0")

        dvs.servers[0].runcmd("ifconfig eth0 inet6 add fc00::2/126")
        dvs.servers[0].runcmd("ip -6 route add default via fc00::1")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds ipv6 route high 90")
        dvs.runcmd("crm config thresholds ipv6 route type free")
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
        dvs.runcmd("config interface startup Ethernet0")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds ipv4 nexthop high 90")
        dvs.runcmd("crm config thresholds ipv4 nexthop type free")
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
        dvs.runcmd("config interface startup Ethernet0")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds ipv6 nexthop high 90")
        dvs.runcmd("crm config thresholds ipv6 nexthop type free")
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
        dvs.runcmd("config interface startup Ethernet0")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds ipv4 neighbor high 90")
        dvs.runcmd("crm config thresholds ipv4 neighbor type free")
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
        dvs.runcmd("config interface startup Ethernet0")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds ipv6 neighbor high 90")
        dvs.runcmd("crm config thresholds ipv6 neighbor type free")
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
        dvs.runcmd("config interface startup Ethernet0")
        dvs.runcmd("config interface startup Ethernet4")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds nexthop group member high 90")
        dvs.runcmd("crm config thresholds nexthop group object type free")
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
        dvs.runcmd("config interface startup Ethernet0")
        dvs.runcmd("config interface startup Ethernet4")

        dvs.runcmd("crm config polling interval 1")

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
        dvs.runcmd("crm config polling interval 2")
        dvs.runcmd("crm config thresholds nexthop group member high 90")
        dvs.runcmd("crm config thresholds nexthop group member type free")
        time.sleep(2)
        check_syslog(dvs, marker, "NEXTHOP_GROUP_MEMBER THRESHOLD_EXCEEDED for TH_FREE",1)

        intf_tbl._del("Ethernet0|10.0.0.0/31")
        intf_tbl._del("Ethernet4|10.0.0.2/31")
        time.sleep(2)

    def test_CrmAcl(self, dvs, testlog):

        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        dvs.runcmd("crm config polling interval 1")
        time.sleep(1)

        bind_ports = ["Ethernet0", "Ethernet4"]

        old_table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_table_used')

        # create ACL table
        ttbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
        ttbl.set("test", fvs)

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

        dvs.runcmd("crm config polling interval 1")
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

    def test_Configure(self, dvs, testlog):

        #polling interval
        dvs.runcmd("crm config polling interval 10")
        time.sleep(2)
        polling_interval = getCrmConfigValue(dvs, 'Config', 'polling_interval')
        assert polling_interval == 10

    def test_Configure_ipv4_route(self, dvs, testlog):

        #ipv4 route low/high threshold/type
        dvs.runcmd("crm config thresholds ipv4 route low 50")
        dvs.runcmd("crm config thresholds ipv4 route high 90")
        dvs.runcmd("crm config thresholds ipv4 route type percentage")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'ipv4_route_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'ipv4_route_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'ipv4_route_threshold_type')
        assert threshold_type == 'percentage'

    def test_Configure_ipv6_route(self, dvs, testlog):

        #ipv6 route low/high threshold/type
        dvs.runcmd("crm config thresholds ipv6 route low 50")
        dvs.runcmd("crm config thresholds ipv6 route high 90")
        dvs.runcmd("crm config thresholds ipv6 route type used")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'ipv6_route_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'ipv6_route_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'ipv6_route_threshold_type')
        assert threshold_type == 'used'

    def test_Configure_ipv4_nexthop(self, dvs, testlog):

        #ipv4 nexthop low/high threshold/type
        dvs.runcmd("crm config thresholds ipv4 nexthop low 50")
        dvs.runcmd("crm config thresholds ipv4 nexthop high 90")
        dvs.runcmd("crm config thresholds ipv4 nexthop type 'percentage'")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'ipv4_nexthop_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'ipv4_nexthop_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'ipv4_nexthop_threshold_type')
        assert threshold_type == 'percentage'

    def test_Configure_ipv6_nexthop(self, dvs, testlog):

        #ipv6 nexthop low/high threshold/type
        dvs.runcmd("crm config thresholds ipv6 nexthop low 50")
        dvs.runcmd("crm config thresholds ipv6 nexthop high 90")
        dvs.runcmd("crm config thresholds ipv6 nexthop type free")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'ipv6_nexthop_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'ipv6_nexthop_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'ipv6_nexthop_threshold_type')
        assert threshold_type == 'free'

    def test_Configure_ipv4_neighbor(self, dvs, testlog):

        #ipv4 neighbor low/high threshold/type
        dvs.runcmd("crm config thresholds ipv4 neighbor low 50")
        dvs.runcmd("crm config thresholds ipv4 neighbor high 90")
        dvs.runcmd("crm config thresholds ipv4 neighbor type percentage")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'ipv4_neighbor_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'ipv4_neighbor_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'ipv4_neighbor_threshold_type')
        assert threshold_type == 'percentage'

    def test_Configure_ipv6_neighbor(self, dvs, testlog):

        #ipv6 neighbor low/high threshold/type
        dvs.runcmd("crm config thresholds ipv6 neighbor low 50")
        dvs.runcmd("crm config thresholds ipv6 neighbor high 90")
        dvs.runcmd("crm config thresholds ipv6 neighbor type used")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'ipv6_neighbor_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'ipv6_neighbor_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'ipv6_neighbor_threshold_type')
        assert threshold_type == 'used'

    def test_Configure_group_member(self, dvs, testlog):

        #nexthop group member low/high threshold/type
        dvs.runcmd("crm config thresholds nexthop group member low 50")
        dvs.runcmd("crm config thresholds nexthop group member high 90")
        dvs.runcmd("crm config thresholds nexthop group member type percentage")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'nexthop_group_member_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'nexthop_group_member_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'nexthop_group_member_threshold_type')
        assert threshold_type == 'percentage'

    def test_Configure_group_object(self, dvs, testlog):

        #nexthop group object low/high threshold/type
        dvs.runcmd("crm config thresholds nexthop group object low 50")
        dvs.runcmd("crm config thresholds nexthop group object high 90")
        dvs.runcmd("crm config thresholds nexthop group object type free")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'nexthop_group_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'nexthop_group_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'nexthop_group_threshold_type')
        assert threshold_type == 'free'

    def test_Configure_acl_table(self, dvs, testlog):

        #thresholds acl table low/high threshold/type
        dvs.runcmd("crm config thresholds acl table low 50")
        dvs.runcmd("crm config thresholds acl table high 90")
        dvs.runcmd("crm config thresholds acl table type percentage")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'acl_table_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'acl_table_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'acl_table_threshold_type')
        assert threshold_type == 'percentage'

    def test_Configure_acl_group(self, dvs, testlog):

        #thresholds acl group low/high threshold/type
        dvs.runcmd("crm config thresholds acl group low 50")
        dvs.runcmd("crm config thresholds acl group high 90")
        dvs.runcmd("crm config thresholds acl group type used")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'acl_group_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'acl_group_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'acl_group_threshold_type')
        assert threshold_type == 'used'

    def test_Configure_acl_group_entry(self, dvs, testlog):

        #thresholds acl group entry low/high threshold/type
        dvs.runcmd("crm config thresholds acl group entry low 50")
        dvs.runcmd("crm config thresholds acl group entry high 90")
        dvs.runcmd("crm config thresholds acl group entry type percentage")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'acl_entry_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'acl_entry_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'acl_entry_threshold_type')
        assert threshold_type == 'percentage'

    def test_Configure_acl_group_counter(self, dvs, testlog):

        #thresholds acl group counter low/high threshold/type
        dvs.runcmd("crm config thresholds acl group counter low 50")
        dvs.runcmd("crm config thresholds acl group counter high 90")
        dvs.runcmd("crm config thresholds acl group counter type free")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'acl_counter_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'acl_counter_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'acl_counter_threshold_type')
        assert threshold_type == 'free'

    def test_Configure_fdb(self, dvs, testlog):

        #thresholds fdb low/high threshold/type
        dvs.runcmd("crm config thresholds fdb low 50")
        dvs.runcmd("crm config thresholds fdb high 90")
        dvs.runcmd("crm config thresholds fdb type percentage")

        time.sleep(2)
        threshold_low = getCrmConfigValue(dvs, 'Config', 'fdb_entry_low_threshold')
        assert threshold_low == 50
        threshold_high = getCrmConfigValue(dvs, 'Config', 'fdb_entry_high_threshold')
        assert threshold_high == 90
        threshold_type = getCrmConfigStr(dvs, 'Config', 'fdb_entry_threshold_type')
        assert threshold_type == 'percentage'
