from swsscommon import swsscommon
import os
import re
import time
import json
import redis


def getCrmCounterValue(dvs, key, counter):

    counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    crm_stats_table = swsscommon.Table(counters_db, 'CRM')

    for k in crm_stats_table.get(key)[1]:
        if k[0] == counter:
            return int(k[1])


def setReadOnlyAttr(dvs, obj, attr, val):

    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "ASIC_STATE:{0}".format(obj))
    keys = tbl.getKeys()

    assert len(keys) == 1

    swVid = keys[0]
    r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB)
    swRid = r.hget("VIDTORID", swVid)

    assert swRid is not None

    ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
    fvp = swsscommon.FieldValuePairs()
    ntf.send("enable_unittests", "true", fvp)
    fvp = swsscommon.FieldValuePairs([(attr, val)])
    key = "SAI_OBJECT_TYPE_SWITCH:" + swRid

    ntf.send("set_ro", key, fvp)


def test_CrmFdbEntry(dvs):

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '1000')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # update available counter
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_available')

    assert new_avail_counter == avail_counter


def test_CrmIpv4Route(dvs):

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '1000')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # remove route and update available counter
    ps._del("2.2.2.0/24")
    dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_route_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmIpv6Route(dvs):

    # Enable IPv6 routing
    dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")
    time.sleep(2)

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|fc00::1/126", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")

    dvs.servers[0].runcmd("ifconfig eth0 inet6 add fc00::2/126")
    dvs.servers[0].runcmd("ip -6 route add default via fc00::1")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY', '1000')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # remove route and update available counter
    ps._del("2001::/64")
    dvs.runcmd("ip -6 neigh del fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_route_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmIpv4Nexthop(dvs):

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY', '1000')

    time.sleep(2)

    # get counters
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_used')
    avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_available')

    # add nexthop and update available counter
    dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # remove nexthop and update available counter
    dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_nexthop_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmIpv6Nexthop(dvs):

    # Enable IPv6 routing
    dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")
    time.sleep(2)

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|fc00::1/126", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY', '1000')

    time.sleep(2)

    # get counters
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_used')
    avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_available')

    # add nexthop and update available counter
    dvs.runcmd("ip -6 neigh replace fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # remove nexthop and update available counter
    dvs.runcmd("ip -6 neigh del fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_nexthop_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmIpv4Neighbor(dvs):

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY', '1000')

    time.sleep(2)

    # get counters
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_used')
    avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_available')

    # add neighbor and update available counter
    dvs.runcmd("ip neigh replace 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # remove neighbor and update available counter
    dvs.runcmd("ip neigh del 10.0.0.1 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv4_neighbor_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmIpv6Neighbor(dvs):

    # Enable IPv6 routing
    dvs.runcmd("sysctl net.ipv6.conf.all.disable_ipv6=0")
    time.sleep(2)

    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    intf_tbl = swsscommon.Table(config_db, "INTERFACE")
    fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
    intf_tbl.set("Ethernet0|fc00::1/126", fvs)
    dvs.runcmd("ifconfig Ethernet0 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY', '1000')

    time.sleep(2)

    # get counters
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_used')
    avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_available')

    # add neighbor and update available counter
    dvs.runcmd("ip -6 neigh replace fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY', '999')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_available')

    assert new_used_counter - used_counter == 1
    assert avail_counter - new_avail_counter == 1

    # remove neighbor and update available counter
    dvs.runcmd("ip -6 neigh del fc00::2 lladdr 11:22:33:44:55:66 dev Ethernet0")
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_ipv6_neighbor_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmNexthopGroup(dvs):

    config_db = swsscommon.dbconnector(swsscommon.config_db, dvs.redis_sock, 0)
    intf_tbl = swsscommon.table(config_db, "INTERFACE")
    fvs = swsscommon.fieldvaluepairs([("NULL","NULL")])
    intf_tbl.set("ethernet0|10.0.0.0/31", fvs)
    intf_tbl.set("ethernet4|10.0.0.2/31", fvs)
    dvs.runcmd("ifconfig ethernet0 up")
    dvs.runcmd("ifconfig ethernet4 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY', '1000')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY', '999')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmNexthopGroupMember(dvs):

    # down, then up to generate port up signal
    dvs.servers[0].runcmd("ip link set down dev eth0") == 0
    dvs.servers[1].runcmd("ip link set down dev eth0") == 0
    dvs.servers[0].runcmd("ip link set up dev eth0") == 0
    dvs.servers[1].runcmd("ip link set up dev eth0") == 0

    config_db = swsscommon.dbconnector(swsscommon.config_db, dvs.redis_sock, 0)
    intf_tbl = swsscommon.table(config_db, "INTERFACE")
    fvs = swsscommon.fieldvaluepairs([("NULL","NULL")])
    intf_tbl.set("ethernet0|10.0.0.0/31", fvs)
    intf_tbl.set("ethernet4|10.0.0.2/31", fvs)
    dvs.runcmd("ifconfig ethernet0 up")
    dvs.runcmd("ifconfig ethernet4 up")

    dvs.runcmd("crm config polling interval 1")

    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY', '1000')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY', '998')

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
    setReadOnlyAttr(dvs, 'SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY', '1000')

    time.sleep(2)

    # get counters
    new_used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_used')
    new_avail_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_nexthop_group_member_available')

    assert new_used_counter == used_counter
    assert new_avail_counter == avail_counter


def test_CrmAcl(dvs):

    db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
    adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

    dvs.runcmd("crm config polling interval 1")

    bind_ports = ["Ethernet0", "Ethernet4"]

    # create ACL table
    ttbl = swsscommon.Table(db, "ACL_TABLE")
    fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
    ttbl.set("test", fvs)

    # create ACL rule
    rtbl = swsscommon.Table(db, "ACL_RULE")
    fvs = swsscommon.FieldValuePairs([("priority", "55"), ("PACKET_ACTION", "FORWARD"), ("L4_SRC_PORT", "65000")])
    rtbl.set("test|acl_test_rule", fvs)

    time.sleep(2)

    table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_table_used')
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

    table_used_counter = getCrmCounterValue(dvs, 'ACL_STATS:INGRESS:PORT', 'crm_stats_acl_table_used')
    assert table_used_counter == 0

