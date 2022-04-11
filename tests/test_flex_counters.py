import time
import pytest

from swsscommon import swsscommon

TUNNEL_TYPE_MAP           =   "COUNTERS_TUNNEL_TYPE_MAP"
NUMBER_OF_RETRIES         =   10
CPU_PORT_OID              = "0x0"
PORT                      = "Ethernet0"
PORT_MAP                  = "COUNTERS_PORT_NAME_MAP"

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
    }
}

@pytest.mark.usefixtures('dvs_port_manager')
class TestFlexCounters(object):

    def setup_dbs(self, dvs):
        self.config_db = dvs.get_config_db()
        self.flex_db = dvs.get_flex_db()
        self.counters_db = dvs.get_counters_db()
        self.app_db = dvs.get_app_db()

    def wait_for_table(self, table):
        for retry in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(table)
            if len(counters_keys) > 0:
                return
            else:
                time.sleep(1)

        assert False, str(table) + " not created in Counters DB"

    def wait_for_table_empty(self, table):
        for retry in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(table)
            if len(counters_keys) == 0:
                return
            else:
                time.sleep(1)

        assert False, str(table) + " is still in Counters DB"

    def wait_for_id_list(self, stat, name, oid):
        for retry in range(NUMBER_OF_RETRIES):
            id_list = self.flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + oid).items()
            if len(id_list) > 0:
                return
            else:
                time.sleep(1)

        assert False, "No ID list for counter " + str(name)

    def wait_for_id_list_remove(self, stat, name, oid):
        for retry in range(NUMBER_OF_RETRIES):
            id_list = self.flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + oid).items()
            if len(id_list) == 0:
                return
            else:
                time.sleep(1)

        assert False, "ID list for counter " + str(name) + " is still there"

    def wait_for_interval_set(self, group, interval):
        interval_value = None
        for retry in range(NUMBER_OF_RETRIES):
            interval_value = self.flex_db.db_connection.hget("FLEX_COUNTER_GROUP_TABLE:" + group, 'POLL_INTERVAL')
            if interval_value == interval:
                return
            else:
                time.sleep(1)

        assert False, "Polling interval is not applied to FLEX_COUNTER_GROUP_TABLE for group {}, expect={}, actual={}".format(group, interval, interval_value)

    def verify_no_flex_counters_tables(self, counter_stat):
        counters_stat_keys = self.flex_db.get_keys("FLEX_COUNTER_TABLE:" + counter_stat)
        assert len(counters_stat_keys) == 0, "FLEX_COUNTER_TABLE:" + str(counter_stat) + " tables exist before enabling the flex counter group"

    def verify_no_flex_counters_tables_after_delete(self, counter_stat):
        for retry in range(NUMBER_OF_RETRIES):
            counters_stat_keys = self.flex_db.get_keys("FLEX_COUNTER_TABLE:" + counter_stat + ":")
            if len(counters_stat_keys) == 0:
                return
            else:
                time.sleep(1)
        assert False, "FLEX_COUNTER_TABLE:" + str(counter_stat) + " tables exist after removing the entries"

    def verify_flex_counters_populated(self, map, stat):
        counters_keys = self.counters_db.db_connection.hgetall(map)
        for counter_entry in counters_keys.items():
            name = counter_entry[0]
            oid = counter_entry[1]
            self.wait_for_id_list(stat, name, oid)

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

    def set_flex_counter_group_status(self, group, map, status='enable'):
        group_stats_entry = {"FLEX_COUNTER_STATUS": status}
        self.config_db.create_entry("FLEX_COUNTER_TABLE", group, group_stats_entry)
        if status == 'enable':
            self.wait_for_table(map)
        else:
            self.wait_for_table_empty(map)

    def set_flex_counter_group_interval(self, key, group, interval):
        group_stats_entry = {"POLL_INTERVAL": interval}
        self.config_db.create_entry("FLEX_COUNTER_TABLE", key, group_stats_entry)
        self.wait_for_interval_set(group, interval)

    @pytest.mark.parametrize("counter_type", counter_group_meta.keys())
    def test_flex_counters(self, dvs, counter_type):
        """
        The test will check there are no flex counters tables on FlexCounter DB when the counters are disabled.
        After enabling each counter group, the test will check the flow of creating flex counters tables on FlexCounter DB.
        For some counter types the MAPS on COUNTERS DB will be created as well after enabling the counter group, this will be also verified on this test.
        """
        self.setup_dbs(dvs)
        meta_data = counter_group_meta[counter_type]
        counter_key = meta_data['key']
        counter_stat = meta_data['group_name']
        counter_map = meta_data['name_map']
        pre_test = meta_data.get('pre_test')
        post_test = meta_data.get('post_test')

        self.verify_no_flex_counters_tables(counter_stat)

        if pre_test:
            cb = getattr(self, pre_test)
            cb(meta_data)

        self.set_flex_counter_group_status(counter_key, counter_map)
        self.verify_flex_counters_populated(counter_map, counter_stat)
        self.set_flex_counter_group_interval(counter_key, counter_stat, '2500')

        if post_test:
            cb = getattr(self, post_test)
            cb(meta_data)

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
        self.set_flex_counter_group_status(meta_data['key'], meta_data['group_name'], 'disable')

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
        self.set_flex_counter_group_status(meta_data['key'], meta_data['group_name'], 'disable')

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

        self.set_flex_counter_group_status(meta_data['key'], meta_data['group_name'], 'disable')
            
    def test_add_remove_ports(self, dvs):
        self.setup_dbs(dvs)
        
        # set flex counter
        counter_key = counter_group_meta['queue_counter']['key']
        counter_stat = counter_group_meta['queue_counter']['group_name']
        counter_map = counter_group_meta['queue_counter']['name_map']
        self.set_flex_counter_group_status(counter_key, counter_map)

        # receive port info
        fvs = self.config_db.get_entry("PORT", PORT)
        assert len(fvs) > 0
        
        # save all the oids of the pg drop counters            
        oid_list = []
        counters_queue_map = self.counters_db.get_entry("COUNTERS_QUEUE_NAME_MAP", "")
        for key, oid in counters_queue_map.items():
            if PORT in key:
                oid_list.append(oid)
                fields = self.flex_db.get_entry("FLEX_COUNTER_TABLE", counter_stat + ":%s" % oid)
                assert len(fields) == 1
        oid_list_len = len(oid_list)

        # get port oid
        port_oid = self.counters_db.get_entry(PORT_MAP, "")[PORT]

        # remove port and verify that it was removed properly
        self.dvs_port.remove_port(PORT)
        dvs.get_asic_db().wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        
        # verify counters were removed from flex counter table
        for oid in oid_list:
            fields = self.flex_db.get_entry("FLEX_COUNTER_TABLE", counter_stat + ":%s" % oid)
            assert len(fields) == 0
        
        # verify that port counter maps were removed from counters db
        counters_queue_map = self.counters_db.get_entry("COUNTERS_QUEUE_NAME_MAP", "")
        for key in counters_queue_map.keys():
            if PORT in key:
                assert False
        
        # add port and wait until the port is added on asic db
        num_of_keys_without_port = len(dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
        
        self.config_db.create_entry("PORT", PORT, fvs)
        
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_of_keys_without_port + 1)
        dvs.get_counters_db().wait_for_fields("COUNTERS_QUEUE_NAME_MAP", "", ["%s:0"%(PORT)])
        
        # verify queue counters were added
        oid_list = []
        counters_queue_map = self.counters_db.get_entry("COUNTERS_QUEUE_NAME_MAP", "")

        for key, oid in counters_queue_map.items():
            if PORT in key:
                oid_list.append(oid)
                fields = self.flex_db.get_entry("FLEX_COUNTER_TABLE", counter_stat + ":%s" % oid)
                assert len(fields) == 1
        # the number of the oids needs to be the same as the original number of oids (before removing a port and adding)
        assert oid_list_len == len(oid_list)
