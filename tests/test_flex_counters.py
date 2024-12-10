import time
import pytest

from dvslib.dvs_flex_counter import TestFlexCountersBase, NUMBER_OF_RETRIES
from swsscommon import swsscommon

TUNNEL_TYPE_MAP           = "COUNTERS_TUNNEL_TYPE_MAP"
ROUTE_TO_PATTERN_MAP      = "COUNTERS_ROUTE_TO_PATTERN_MAP"
CPU_PORT_OID              = "0x0"

counter_group_meta = {
    'port_counter': {
        'key': 'PORT',
        'group_name': 'PORT_STAT_COUNTER',
        'name_map': 'COUNTERS_PORT_NAME_MAP',
        'post_test':  'post_port_counter_test',
    },
    'queue_counter': {
        'key': 'QUEUE',
        'group_name': 'QUEUE_STAT_COUNTER',
        'name_map': 'COUNTERS_QUEUE_NAME_MAP',
    },
    'queue_watermark_counter': {
        'key': 'QUEUE_WATERMARK',
        'group_name': 'QUEUE_WATERMARK_STAT_COUNTER',
        'name_map': 'COUNTERS_QUEUE_NAME_MAP',
    },
    'rif_counter': {
        'key': 'RIF',
        'group_name': 'RIF_STAT_COUNTER',
        'name_map': 'COUNTERS_RIF_NAME_MAP',
        'pre_test': 'pre_rif_counter_test',
        'post_test':  'post_rif_counter_test',
    },
    'buffer_pool_watermark_counter': {
        'key': 'BUFFER_POOL_WATERMARK',
        'group_name': 'BUFFER_POOL_WATERMARK_STAT_COUNTER',
        'name_map': 'COUNTERS_BUFFER_POOL_NAME_MAP',
    },
    'port_buffer_drop_counter': {
        'key': 'PORT_BUFFER_DROP',
        'group_name': 'PORT_BUFFER_DROP_STAT',
        'name_map': 'COUNTERS_PORT_NAME_MAP',
    },
    'pg_drop_counter': {
        'key': 'PG_DROP',
        'group_name': 'PG_DROP_STAT_COUNTER',
        'name_map': 'COUNTERS_PG_NAME_MAP',
    },
    'pg_watermark_counter': {
        'key': 'PG_WATERMARK',
        'group_name': 'PG_WATERMARK_STAT_COUNTER',
        'name_map': 'COUNTERS_PG_NAME_MAP',
    },
    'trap_flow_counter': {
        'key': 'FLOW_CNT_TRAP',
        'group_name': 'HOSTIF_TRAP_FLOW_COUNTER',
        'name_map': 'COUNTERS_TRAP_NAME_MAP',
        'post_test':  'post_trap_flow_counter_test',
    },
    'tunnel_counter': {
        'key': 'TUNNEL',
        'group_name': 'TUNNEL_STAT_COUNTER',
        'name_map': 'COUNTERS_TUNNEL_NAME_MAP',
        'pre_test': 'pre_vxlan_tunnel_counter_test',
        'post_test':  'post_vxlan_tunnel_counter_test',
    },
    'acl_counter': {
        'key': 'ACL',
        'group_name': 'ACL_STAT_COUNTER',
        'name_map': 'ACL_COUNTER_RULE_MAP',
        'pre_test': 'pre_acl_tunnel_counter_test',
        'post_test':  'post_acl_tunnel_counter_test',
    },
    'route_flow_counter': {
        'key': 'FLOW_CNT_ROUTE',
        'group_name': 'ROUTE_FLOW_COUNTER',
        'name_map': 'COUNTERS_ROUTE_NAME_MAP',
        'pre_test': 'pre_route_flow_counter_test',
        'post_test':  'post_route_flow_counter_test',
    }
}


class TestFlexCounters(TestFlexCountersBase):

    def wait_for_buffer_pg_queue_counter(self, map, port, index, isSet):
        for retry in range(NUMBER_OF_RETRIES):
            counter_oid = self.counters_db.db_connection.hget(map, port + ':' + index)
            if (isSet and counter_oid):
                return counter_oid
            elif (not isSet and not counter_oid):
                return None
            else:
                time.sleep(1)

        assert False, "Counter not {} for port: {}, type: {}, index: {}".format("created" if isSet else "removed", port, map, index)

    def verify_no_flex_counters_tables_after_delete(self, counter_stat):
        for retry in range(NUMBER_OF_RETRIES):
            counters_stat_keys = self.flex_db.get_keys("FLEX_COUNTER_TABLE:" + counter_stat + ":")
            if len(counters_stat_keys) == 0:
                return
            else:
                time.sleep(1)
        assert False, "FLEX_COUNTER_TABLE:" + str(counter_stat) + " tables exist after removing the entries"

    def verify_tunnel_type_vxlan(self, meta_data, type_map):
        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        for counter_entry in counters_keys.items():
            oid = counter_entry[1]
            fvs = self.counters_db.get_entry(type_map, "")
            assert fvs != {}
            assert fvs.get(oid) == "SAI_TUNNEL_TYPE_VXLAN"

    def verify_only_phy_ports_created(self, meta_data):
        port_counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        port_counters_stat_keys = self.flex_db.get_keys("FLEX_COUNTER_TABLE:" + meta_data['group_name'])
        for port_stat in port_counters_stat_keys:
            assert port_stat in dict(port_counters_keys.items()).values(), "Non PHY port created on PORT_STAT_COUNTER group: {}".format(port_stat)

    def set_only_config_db_buffers_field(self, value):
        fvs = {'create_only_config_db_buffers' : value}
        self.config_db.update_entry("DEVICE_METADATA", "localhost", fvs)

    @pytest.mark.parametrize("counter_type", counter_group_meta.keys())
    def test_flex_counters(self, dvs, counter_type):
        self.verify_flex_counter_flow(dvs, counter_group_meta[counter_type])

    def pre_rif_counter_test(self, meta_data):
        self.config_db.db_connection.hset('INTERFACE|Ethernet0', "NULL", "NULL")
        self.config_db.db_connection.hset('INTERFACE|Ethernet0|192.168.0.1/24', "NULL", "NULL")

    def pre_vxlan_tunnel_counter_test(self, meta_data):
        self.config_db.db_connection.hset("VLAN|Vlan10", "vlanid", "10")
        self.config_db.db_connection.hset("VXLAN_TUNNEL|vtep1", "src_ip", "1.1.1.1")
        self.config_db.db_connection.hset("VXLAN_TUNNEL_MAP|vtep1|map_100_Vlan10", "vlan", "Vlan10")
        self.config_db.db_connection.hset("VXLAN_TUNNEL_MAP|vtep1|map_100_Vlan10", "vni", "100")

    def pre_acl_tunnel_counter_test(self, meta_data):
        self.config_db.create_entry('ACL_TABLE', 'DATAACL',
            {
                'STAGE': 'INGRESS',
                'PORTS': 'Ethernet0',
                'TYPE': 'L3'
            }
        )
        self.config_db.create_entry('ACL_RULE', 'DATAACL|RULE0',
            {
                'ETHER_TYPE': '2048',
                'PACKET_ACTION': 'FORWARD',
                'PRIORITY': '9999'
            }
        )

    def pre_route_flow_counter_test(self, meta_data):
        dvs = meta_data['dvs']
        self.config_db.create_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16',
            {
                'max_match_count': '30'
            }
        )
        self.config_db.create_entry('FLOW_COUNTER_ROUTE_PATTERN', '2000::/64',
            {
                'max_match_count': '30'
            }
        )

        self.create_l3_intf("Ethernet0", "")
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.set_admin_status("Ethernet0", "up")
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")
        dvs.servers[0].runcmd("ping -c 1 10.0.0.1")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 1.1.1.0/24 10.0.0.1\"")

        self.create_l3_intf("Ethernet4", "")
        self.set_admin_status("Ethernet4", "up")
        self.add_ip_address("Ethernet4", "2001::1/64")
        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=1")
        dvs.servers[1].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route add default via 2001::1")
        time.sleep(2)
        dvs.servers[1].runcmd("ping -6 -c 1 2001::1")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 2000::/64 2001::2\"")

    def post_rif_counter_test(self, meta_data):
        self.config_db.db_connection.hdel('INTERFACE|Ethernet0|192.168.0.1/24', "NULL")

    def post_port_counter_test(self, meta_data):
        self.verify_only_phy_ports_created(meta_data)

    def post_trap_flow_counter_test(self, meta_data):
        """Post verification for test_flex_counters for trap_flow_counter. Steps:
               1. Disable test_flex_counters
               2. Verify name map and counter ID list are cleared
               3. Clear trap ids to avoid affecting further test cases

        Args:
            meta_data (object): flex counter meta data
        """
        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')

        for counter_entry in counters_keys.items():
            self.wait_for_id_list_remove(meta_data['group_name'], counter_entry[0], counter_entry[1])
        self.wait_for_table_empty(meta_data['name_map'])

    def post_vxlan_tunnel_counter_test(self, meta_data):
        self.verify_tunnel_type_vxlan(meta_data, TUNNEL_TYPE_MAP)
        self.config_db.delete_entry("VLAN","Vlan10")
        self.config_db.delete_entry("VLAN_TUNNEL","vtep1")
        self.config_db.delete_entry("VLAN_TUNNEL_MAP","vtep1|map_100_Vlan10")
        self.verify_no_flex_counters_tables_after_delete(meta_data['group_name'])

    def post_acl_tunnel_counter_test(self, meta_data):
        self.config_db.delete_entry('ACL_RULE', 'DATAACL|RULE0')
        self.config_db.delete_entry('ACL_TABLE', 'DATAACL')

    def post_route_flow_counter_test(self, meta_data):
        dvs = meta_data['dvs']
        # Verify prefix to route pattern name map
        self.wait_for_table(ROUTE_TO_PATTERN_MAP)

        # Remove route pattern and verify related couters are removed
        v4_name_map_key = '1.1.1.0/24'
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], v4_name_map_key)
        assert counter_oid
        self.config_db.delete_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16')
        self.wait_for_id_list_remove(meta_data['group_name'], v4_name_map_key, counter_oid)
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], v4_name_map_key)
        assert not counter_oid
        route_pattern = self.counters_db.db_connection.hget(ROUTE_TO_PATTERN_MAP, v4_name_map_key)
        assert not route_pattern

        # Disable route flow counter and verify all counters are removed
        v6_name_map_key = '2000::/64'
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], v6_name_map_key)
        assert counter_oid
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')
        self.wait_for_id_list_remove(meta_data['group_name'], v6_name_map_key, counter_oid)
        self.wait_for_table_empty(meta_data['name_map'])
        self.wait_for_table_empty(ROUTE_TO_PATTERN_MAP)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route {} 10.0.0.1\"".format(v4_name_map_key))
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_l3_intf("Ethernet0")
        self.set_admin_status("Ethernet0", "down")
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 2000::/64 2001::2\"")
        self.remove_ip_address("Ethernet4", "2001::1/64")
        self.remove_l3_intf("Ethernet4")
        self.set_admin_status("Ethernet4", "down")
        dvs.servers[1].runcmd("ip -6 route del default dev eth0")
        dvs.servers[1].runcmd("ip -6 address del 2001::2/64 dev eth0")
        self.config_db.delete_entry('FLOW_COUNTER_ROUTE_PATTERN', '2000::/64')

    def test_add_remove_trap(self, dvs):
        """Test steps:
               1. Enable trap_flow_counter
               2. Remove a COPP trap
               3. Verify counter is automatically unbind
               4. Add the COPP trap back
               5. Verify counter is added back

        Args:
            dvs (object): virtual switch object
        """
        self.setup_dbs(dvs)
        meta_data = counter_group_meta['trap_flow_counter']
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        removed_trap = None
        changed_group = None
        trap_ids = None
        copp_groups = self.app_db.db_connection.keys('COPP_TABLE:*')
        for copp_group in copp_groups:
            trap_ids = self.app_db.db_connection.hget(copp_group, 'trap_ids')
            if trap_ids and ',' in trap_ids:
                trap_ids = [x.strip() for x in trap_ids.split(',')]
                removed_trap = trap_ids.pop()
                changed_group = copp_group.split(':')[1]
                break

        if not removed_trap:
            pytest.skip('There is not copp group with more than 1 traps, skip rest of the test')

        oid = None
        for _ in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
            if removed_trap in counters_keys:
                oid = counters_keys[removed_trap]
                break
            else:
                time.sleep(1)

        assert oid, 'trap counter is not created for {}'.format(removed_trap)
        self.wait_for_id_list(meta_data['group_name'], removed_trap, oid)

        app_copp_table = swsscommon.ProducerStateTable(self.app_db.db_connection, 'COPP_TABLE')
        app_copp_table.set(changed_group, [('trap_ids', ','.join(trap_ids))])
        self.wait_for_id_list_remove(meta_data['group_name'], removed_trap, oid)
        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        assert removed_trap not in counters_keys

        trap_ids.append(removed_trap)
        app_copp_table.set(changed_group, [('trap_ids', ','.join(trap_ids))])

        oid = None
        for _ in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
            if removed_trap in counters_keys:
                oid = counters_keys[removed_trap]
                break
            else:
                time.sleep(1)

        assert oid, 'Add trap {}, but trap counter is not created'.format(removed_trap)
        self.wait_for_id_list(meta_data['group_name'], removed_trap, oid)
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')

    def test_remove_trap_group(self, dvs):
        """Remove trap group and verify that all related trap counters are removed

        Args:
            dvs (object): virtual switch object
        """
        self.setup_dbs(dvs)
        meta_data = counter_group_meta['trap_flow_counter']
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        removed_group = None
        trap_ids = None
        copp_groups = self.app_db.db_connection.keys('COPP_TABLE:*')
        for copp_group in copp_groups:
            trap_ids = self.app_db.db_connection.hget(copp_group, 'trap_ids')
            if trap_ids and trap_ids.strip():
                removed_group = copp_group.split(':')[1]
                break

        if not removed_group:
            pytest.skip('There is not copp group with at least 1 traps, skip rest of the test')

        trap_ids = [x.strip() for x in trap_ids.split(',')]
        for _ in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
            found = True
            for trap_id in trap_ids:
                if trap_id not in counters_keys:
                    found = False
                    break
            if found:
                break
            else:
                time.sleep(1)

        assert found, 'Not all trap id found in name map'
        for trap_id in trap_ids:
            self.wait_for_id_list(meta_data['group_name'], trap_id, counters_keys[trap_id])

        app_copp_table = swsscommon.ProducerStateTable(self.app_db.db_connection, 'COPP_TABLE')
        app_copp_table._del(removed_group)

        for trap_id in trap_ids:
            self.wait_for_id_list_remove(meta_data['group_name'], trap_id, counters_keys[trap_id])

        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        for trap_id in trap_ids:
            assert trap_id not in counters_keys

        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')

    def test_update_route_pattern(self, dvs):
        self.setup_dbs(dvs)
        self.config_db.create_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16',
            {
                'max_match_count': '30'
            }
        )
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")
        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")
        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 1.1.1.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.3\"")

        meta_data = counter_group_meta['route_flow_counter']
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])
        self.wait_for_table(meta_data['name_map'])
        self.wait_for_table(ROUTE_TO_PATTERN_MAP)
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], '1.1.1.0/24')
        self.wait_for_id_list(meta_data['group_name'], '1.1.1.0/24', counter_oid)
        assert not self.counters_db.db_connection.hget(meta_data['name_map'], '2.2.2.0/24')
        assert not self.counters_db.db_connection.hget(ROUTE_TO_PATTERN_MAP, '2.2.2.0/24')

        self.config_db.delete_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16')
        self.wait_for_id_list_remove(meta_data['group_name'], '1.1.1.0/24', counter_oid)
        self.wait_for_table_empty(meta_data['name_map'])
        self.wait_for_table_empty(ROUTE_TO_PATTERN_MAP)
        assert not self.counters_db.db_connection.hget(meta_data['name_map'], '1.1.1.0/24')
        assert not self.counters_db.db_connection.hget(ROUTE_TO_PATTERN_MAP, '1.1.1.0/24')

        self.config_db.create_entry('FLOW_COUNTER_ROUTE_PATTERN', '2.2.0.0/16',
            {
                'max_match_count': '30'
            }
        )
        self.wait_for_table(meta_data['name_map'])
        self.wait_for_table(ROUTE_TO_PATTERN_MAP)
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], '2.2.2.0/24')
        self.wait_for_id_list(meta_data['group_name'], '2.2.2.0/24', counter_oid)

        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')
        self.wait_for_id_list_remove(meta_data['group_name'], '2.2.2.0/24', counter_oid)
        self.wait_for_table_empty(meta_data['name_map'])
        self.wait_for_table_empty(ROUTE_TO_PATTERN_MAP)

        self.config_db.delete_entry('FLOW_COUNTER_ROUTE_PATTERN', '2.2.0.0/16')
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route {} 10.0.0.1\"".format('1.1.1.0/24'))
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route {} 10.0.0.3\"".format('2.2.2.0/24'))

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")

    def test_add_remove_route_flow_counter(self, dvs):
        self.setup_dbs(dvs)
        self.config_db.create_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16',
            {
                'max_match_count': '30'
            }
        )
        meta_data = counter_group_meta['route_flow_counter']
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], check_name_map=False)

        self.create_l3_intf("Ethernet0", "")
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.set_admin_status("Ethernet0", "up")
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")
        dvs.servers[0].runcmd("ping -c 1 10.0.0.1")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 1.1.1.0/24 10.0.0.1\"")

        self.wait_for_table(meta_data['name_map'])
        self.wait_for_table(ROUTE_TO_PATTERN_MAP)
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], '1.1.1.0/24')
        self.wait_for_id_list(meta_data['group_name'], '1.1.1.0/24', counter_oid)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route {} 10.0.0.1\"".format('1.1.1.0/24'))
        self.wait_for_id_list_remove(meta_data['group_name'], '1.1.1.0/24', counter_oid)
        self.wait_for_table_empty(meta_data['name_map'])
        self.wait_for_table_empty(ROUTE_TO_PATTERN_MAP)

        self.config_db.delete_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16')
        self.set_flex_counter_group_status(meta_data['key'], meta_data['group_name'], 'disable')

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")

        self.set_admin_status("Ethernet0", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

    def test_router_flow_counter_max_match_count(self, dvs):
        self.setup_dbs(dvs)
        self.config_db.create_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16',
            {
                'max_match_count': '1'
            }
        )
        meta_data = counter_group_meta['route_flow_counter']
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], check_name_map=False)
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")
        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")
        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 1.1.1.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 1.1.2.0/24 10.0.0.3\"")

        self.wait_for_table(meta_data['name_map'])
        self.wait_for_table(ROUTE_TO_PATTERN_MAP)
        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], '1.1.1.0/24')
        self.wait_for_id_list(meta_data['group_name'], '1.1.1.0/24', counter_oid)
        assert not self.counters_db.db_connection.hget(meta_data['name_map'], '1.1.2.0/24')
        self.config_db.update_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16',
            {
                'max_match_count': '2'
            }
        )
        for _ in range(NUMBER_OF_RETRIES):
            counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], '1.1.2.0/24')
            if not counter_oid:
                time.sleep(1)
            else:
                break
        assert counter_oid
        self.wait_for_id_list(meta_data['group_name'], '1.1.2.0/24', counter_oid)

        self.config_db.update_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16',
            {
                'max_match_count': '1'
            }
        )

        for _ in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
            if len(counters_keys) == 1:
                break
            else:
                time.sleep(1)

        assert len(counters_keys) == 1

        to_remove = '1.1.2.0/24' if '1.1.2.0/24' in counters_keys else '1.1.1.0/24'
        to_remove_nexthop = '10.0.0.3' if '1.1.2.0/24' in counters_keys else '10.0.0.1'
        to_bound = '1.1.2.0/24' if '1.1.1.0/24' == to_remove else '1.1.1.0/24'
        to_bound_nexthop = '10.0.0.1' if '1.1.2.0/24' in counters_keys else '10.0.0.3'

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route {} {}\"".format(to_remove, to_remove_nexthop))
        for _ in range(NUMBER_OF_RETRIES):
            counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], to_bound)
            if not counter_oid:
                time.sleep(1)
            else:
                break
        assert counter_oid
        self.wait_for_id_list(meta_data['group_name'], to_bound, counter_oid)
        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        assert to_remove not in counters_keys
        assert to_bound in counters_keys
        counters_keys = self.counters_db.db_connection.hgetall(ROUTE_TO_PATTERN_MAP)
        assert to_remove not in counters_keys
        assert to_bound in counters_keys

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route {} {}\"".format(to_bound, to_bound_nexthop))

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")
        self.config_db.delete_entry('FLOW_COUNTER_ROUTE_PATTERN', '1.1.0.0/16')

    def create_l3_intf(self, interface, vrf_name):
        if len(vrf_name) == 0:
            self.config_db.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.config_db.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

    def remove_l3_intf(self, interface):
        self.config_db.delete_entry("INTERFACE", interface)

    def add_ip_address(self, interface, ip):
        self.config_db.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.config_db.delete_entry("INTERFACE", interface + "|" + ip)

    def set_admin_status(self, interface, status):
        self.config_db.update_entry("PORT", interface, {"admin_status": status})

    @pytest.mark.parametrize('counter_type_id', [('queue_counter', '8'), ('pg_drop_counter', '7')])
    def test_create_only_config_db_buffers_false(self, dvs, counter_type_id):
        """
        Test steps:
            1. By default the configuration knob 'create_only_config_db_value' is missing.
            2. Get the counter OID for the interface 'Ethernet0', queue 8 or PG 7, from the counters database.
            3. Perform assertions based on the 'create_only_config_db_value':
                - If 'create_only_config_db_value' is 'false' or does not exist, assert that the counter OID has a valid OID value.

        Args:
            dvs (object): virtual switch object
            counter_type (str): The type of counter being tested
        """
        self.setup_dbs(dvs)
        counter_type, index = counter_type_id
        meta_data = counter_group_meta[counter_type]
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], 'Ethernet0:' + index)
        assert counter_oid is not None, "Counter OID should have a valid OID value when create_only_config_db_value is 'false' or does not exist"

    def test_create_remove_buffer_pg_watermark_counter(self, dvs):
        """
        Test steps:
            1. Reset config_db
            2. Set 'create_only_config_db_buffers' to 'true'
            3. Enable PG flex counters.
            4. Configure new buffer prioriy group for a port
            5. Verify counter is automatically created
            6. Remove the new buffer prioriy group for the port
            7. Verify counter is automatically removed

        Args:
            dvs (object): virtual switch object
        """
        dvs.restart()
        self.setup_dbs(dvs)
        self.set_only_config_db_buffers_field('true')
        meta_data = counter_group_meta['pg_watermark_counter']

        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|1', {'profile': 'ingress_lossy_profile'})
        counter_oid = self.wait_for_buffer_pg_queue_counter(meta_data['name_map'], 'Ethernet0', '1', True)
        self.wait_for_id_list(meta_data['group_name'], "Ethernet0", counter_oid)

        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|1')
        self.wait_for_buffer_pg_queue_counter(meta_data['name_map'], 'Ethernet0', '1', False)
        self.wait_for_id_list_remove(meta_data['group_name'], "Ethernet0", counter_oid)

    @pytest.mark.parametrize('counter_type_id', [('queue_counter', '8'), ('pg_drop_counter', '7')])
    def test_create_only_config_db_buffers_true(self, dvs, counter_type_id):
        """
        Test steps:
            1. The 'create_only_config_db_buffers' was set to 'true' by previous test.
            2. Get the counter OID for the interface 'Ethernet0', queue 8 or PG 7, from the counters database.
            3. Perform assertions based on the 'create_only_config_db_value':
                - If 'create_only_config_db_value' is 'true', assert that the counter OID is None.

        Args:
            dvs (object): virtual switch object
            counter_type (str): The type of counter being tested
        """
        counter_type, index = counter_type_id
        self.setup_dbs(dvs)
        meta_data = counter_group_meta[counter_type]
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        counter_oid = self.counters_db.db_connection.hget(meta_data['name_map'], 'Ethernet0:' + index)
        assert counter_oid is None, "Counter OID should be None when create_only_config_db_value is 'true'"

    def test_create_remove_buffer_queue_counter(self, dvs):
        """
        Test steps:
            1. Enable Queue flex counters.
            2. Configure new buffer queue for a port
            3. Verify counter is automatically created
            4. Remove the new buffer queue for the port
            5. Verify counter is automatically removed

        Args:
            dvs (object): virtual switch object
        """
        self.setup_dbs(dvs)
        meta_data = counter_group_meta['queue_counter']

        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        self.config_db.update_entry('BUFFER_QUEUE', 'Ethernet0|8', {'profile': 'egress_lossless_profile'})
        counter_oid = self.wait_for_buffer_pg_queue_counter(meta_data['name_map'], 'Ethernet0', '8', True)
        self.wait_for_id_list(meta_data['group_name'], "Ethernet0", counter_oid)

        self.config_db.delete_entry('BUFFER_QUEUE', 'Ethernet0|8')
        self.wait_for_buffer_pg_queue_counter(meta_data['name_map'], 'Ethernet0', '8', False)
        self.wait_for_id_list_remove(meta_data['group_name'], "Ethernet0", counter_oid)

    def test_create_remove_buffer_watermark_queue_pg_counter(self, dvs):
        """
        Test steps:
            1. Enable Queue/Watermark/PG-drop flex counters.
            2. Configure new buffer queue for a port
            3. Verify counters is automatically created
            4. Remove the new buffer queue for the port
            5. Verify counters is automatically removed

        Args:
            dvs (object): virtual switch object
        """
        self.setup_dbs(dvs)

        # set flex counter
        for counterpoll_type, meta_data in counter_group_meta.items():
            if 'queue' in counterpoll_type or 'pg' in counterpoll_type:
                self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'])

        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|7', {'profile': 'ingress_lossy_profile'})
        self.config_db.update_entry('BUFFER_QUEUE', 'Ethernet0|8', {'profile': 'egress_lossless_profile'})

        for counterpoll_type, meta_data in counter_group_meta.items():
            if 'queue' in counterpoll_type or 'pg' in counterpoll_type:
                index = '8' if 'queue' in counterpoll_type else '7'
                counter_oid = self.wait_for_buffer_pg_queue_counter(meta_data['name_map'], 'Ethernet0', index, True)
                self.wait_for_id_list(meta_data['group_name'], "Ethernet0", counter_oid)

        self.config_db.delete_entry('BUFFER_QUEUE', 'Ethernet0|8')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|7')
        for counterpoll_type, meta_data in counter_group_meta.items():
            if 'queue' in counterpoll_type or 'pg' in counterpoll_type:
                index = '8' if 'queue' in counterpoll_type else '7'
                self.wait_for_buffer_pg_queue_counter(meta_data['name_map'], 'Ethernet0', index, False)
                self.wait_for_id_list_remove(meta_data['group_name'], "Ethernet0", counter_oid)
