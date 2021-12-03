# Lint as: python3
from swsscommon import swsscommon

import pytest
import util
import acl


def get_exist_entry(dvs, table):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, table)
    entries = list(tbl.getKeys())
    return entries[0]


def verify_selected_attr_vals(db, table, key, expected_attrs):
    tbl = swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert key in keys, "The desired key is not presented"

    status, fvs = tbl.get(key)
    assert status, "Got an error when get a key"

    fv_dict = dict(fvs)

    for attr_name, expected_val in expected_attrs:
        assert attr_name in fv_dict, "Attribute %s not found in %s" % (attr_name, key)
        assert fv_dict[attr_name] == expected_val, "Wrong value %s for the attribute %s = %s" % (
                    fv_dict[attr_name],
                    attr_name,
                    expected_val,
                )


class TestP4RTAcl(object):
    def _set_up(self, dvs):
        self._p4rt_acl_table_definition_obj = acl.P4RtAclTableDefinitionWrapper()
        self._p4rt_acl_group_obj = acl.P4RtAclGroupWrapper()
        self._p4rt_acl_group_member_obj = acl.P4RtAclGroupMemberWrapper()
        self._p4rt_acl_rule_obj = acl.P4RtAclRuleWrapper()
        self._p4rt_acl_counter_obj = acl.P4RtAclCounterWrapper()
        self._p4rt_acl_meter_obj = acl.P4RtAclMeterWrapper()
        self._p4rt_trap_group_obj = acl.P4RtTrapGroupWrapper()
        self._p4rt_user_trap_obj = acl.P4RtUserDefinedTrapWrapper()
        self._p4rt_hostif_obj = acl.P4RtHostifWrapper()
        self._p4rt_hostif_table_entry_obj = acl.P4RtHostifTableEntryWrapper()
        self._p4rt_udf_group_obj = acl.P4RtUdfGroupWrapper()
        self._p4rt_udf_match_obj = acl.P4RtUdfMatchWrapper()
        self._p4rt_udf_obj = acl.P4RtUdfWrapper()

        self._p4rt_acl_group_member_obj.set_up_databases(dvs)
        self._p4rt_acl_group_obj.set_up_databases(dvs)
        self._p4rt_acl_table_definition_obj.set_up_databases(dvs)
        self._p4rt_acl_rule_obj.set_up_databases(dvs)
        self._p4rt_acl_counter_obj.set_up_databases(dvs)
        self._p4rt_acl_meter_obj.set_up_databases(dvs)
        self._p4rt_trap_group_obj.set_up_databases(dvs)
        self._p4rt_user_trap_obj.set_up_databases(dvs)
        self._p4rt_hostif_obj.set_up_databases(dvs)
        self._p4rt_hostif_table_entry_obj.set_up_databases(dvs)
        self._p4rt_udf_group_obj.set_up_databases(dvs)
        self._p4rt_udf_match_obj.set_up_databases(dvs)
        self._p4rt_udf_obj.set_up_databases(dvs)

        self.response_consumer = swsscommon.NotificationConsumer(
            self._p4rt_acl_table_definition_obj.appl_db, "APPL_DB_P4RT_TABLE_RESPONSE_CHANNEL"
        )

    @pytest.mark.skip(reason="p4orch is not enabled")
    def test_AclRulesAddUpdateDelPass(self, dvs, testlog):
        # initialize ACL table objects and database connectors
        self._set_up(dvs)

        # maintain list of original Application and ASIC DB entries before adding
        # new ACL table
        original_appl_acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
        )
        original_appl_state_acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
        )
        original_asic_acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.asic_db,
            self._p4rt_acl_table_definition_obj.ASIC_DB_TBL_NAME,
        )
        original_asic_udf_groups = util.get_keys(
            self._p4rt_udf_group_obj.asic_db, self._p4rt_udf_group_obj.ASIC_DB_TBL_NAME
        )
        original_asic_udf_matches = util.get_keys(
            self._p4rt_udf_match_obj.asic_db, self._p4rt_udf_match_obj.ASIC_DB_TBL_NAME
        )
        original_asic_udfs = util.get_keys(
            self._p4rt_udf_obj.asic_db, self._p4rt_udf_obj.ASIC_DB_TBL_NAME
        )

        # query ASIC database for ACL groups
        acl_groups_asic_keys = util.get_keys(
            self._p4rt_acl_group_obj.asic_db, self._p4rt_acl_group_obj.ASIC_DB_TBL_NAME
        )
        assert (
            len(acl_groups_asic_keys) == 3
        )  # INGRESS, EGRESS and PRE_INGRESS bind to SWITCH
        switch_oid = get_exist_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        # Ingress
        ingress_group_oids = self._p4rt_acl_group_obj.get_group_oids_by_stage(
            acl.INGRESS_STAGE
        )
        assert len(ingress_group_oids) == 1
        # Egress
        egress_group_oids = self._p4rt_acl_group_obj.get_group_oids_by_stage(
            acl.EGRESS_STAGE
        )
        assert len(egress_group_oids) == 1
        # Pre_ingress
        pre_ingress_group_oids = self._p4rt_acl_group_obj.get_group_oids_by_stage(
            acl.PRE_INGRESS_STAGE
        )
        assert len(pre_ingress_group_oids) == 1
        verify_selected_attr_vals(
            self._p4rt_acl_group_obj.asic_db,
            "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH",
            switch_oid,
            [("SAI_SWITCH_ATTR_PRE_INGRESS_ACL", pre_ingress_group_oids[0]),
            ("SAI_SWITCH_ATTR_INGRESS_ACL",ingress_group_oids[0]),
            ("SAI_SWITCH_ATTR_EGRESS_ACL", egress_group_oids[0])],
        )

        # Verify APP DB trap groups for QOS_QUEUE
        genetlink_name = "genl_packet"
        genetlink_mcgrp_name = "packets"

        for queue_num in range(1, 9):
            attr_list = [
                (self._p4rt_trap_group_obj.QUEUE, str(queue_num)),
                (self._p4rt_trap_group_obj.HOSTIF_NAME, genetlink_name),
                (
                    self._p4rt_trap_group_obj.HOSTIF_GENETLINK_MCGRP_NAME,
                    genetlink_mcgrp_name,
                ),
            ]

            # query application database for trap group
            (status, fvs) = util.get_key(
                self._p4rt_trap_group_obj.appl_db,
                self._p4rt_trap_group_obj.APP_DB_TBL_NAME,
                self._p4rt_trap_group_obj.TBL_NAME_PREFIX + str(queue_num),
            )
            assert status == True
            util.verify_attr(fvs, attr_list)

        # create ACL table
        table_name = "ACL_PUNT_TABLE_RULE_TEST"
        stage = "INGRESS"
        priority = "234"
        size = "123"
        ether_type = '{"kind":"sai_field","sai_field":"SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE","format":"HEX_STRING","bitwidth":8}'
        ether_dst = '{"kind":"sai_field","sai_field":"SAI_ACL_TABLE_ATTR_FIELD_DST_MAC","format":"MAC","bitwidth":48}'
        is_ip = '{"kind":"sai_field","sai_field":"SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE/IP","format":"HEX_STRING","bitwidth":1}'
        is_ipv4 = '{"kind":"sai_field","sai_field":"SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE/IPV4ANY","format":"HEX_STRING","bitwidth":1}'
        is_ipv6 = '{"kind":"sai_field","sai_field":"SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE/IPV6ANY","format":"HEX_STRING","bitwidth":1}'
        is_arp = '{"kind":"sai_field","sai_field":"SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE/ARP","format":"HEX_STRING","bitwidth":1}'
        arp_tpa = """{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,
                  \"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"bitwidth\":16,\"offset\":24},
                  {\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"bitwidth\":16,\"offset\":26}]}
               """
        src_ipv6_64bit = """{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,
                            \"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},
                            {\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":32}]}
                        """
        meter_unit = "PACKETS"
        counter_unit = "BOTH"
        copy_and_set_tc = '[{"action":"SAI_PACKET_ACTION_COPY"},{"action":"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC","param":"traffic_class"}]'
        punt_and_set_tc = '[{"action":"SAI_PACKET_ACTION_TRAP","packet_color":"SAI_PACKET_COLOR_RED"},{"action":"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC","param":"traffic_class"}]'
        qos_queue = '[{"action":"SAI_PACKET_ACTION_TRAP"},{"action":"QOS_QUEUE","param":"cpu_queue"}]'

        attr_list = [
            (self._p4rt_acl_table_definition_obj.STAGE_FIELD, stage),
            (self._p4rt_acl_table_definition_obj.PRIORITY_FIELD, priority),
            (self._p4rt_acl_table_definition_obj.SIZE_FIELD, size),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_ETHER_DST, ether_dst),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_ETHER_TYPE, ether_type),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_IS_IP, is_ip),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_IS_IPV4, is_ipv4),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_IS_IPV6, is_ipv6),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_IS_ARP, is_arp),
            (
                self._p4rt_acl_table_definition_obj.MATCH_FIELD_SRC_IPV6_64BIT,
                src_ipv6_64bit,
            ),
            (self._p4rt_acl_table_definition_obj.MATCH_FIELD_ARP_TPA, arp_tpa),
            (
                self._p4rt_acl_table_definition_obj.ACTION_COPY_AND_SET_TC,
                copy_and_set_tc,
            ),
            (
                self._p4rt_acl_table_definition_obj.ACTION_PUNT_AND_SET_TC,
                punt_and_set_tc,
            ),
            (self._p4rt_acl_table_definition_obj.ACTION_SET_QOS_QUEUE, qos_queue),
            (self._p4rt_acl_table_definition_obj.METER_UNIT, meter_unit),
            (self._p4rt_acl_table_definition_obj.COUNTER_UNIT, counter_unit),
        ]

        self._p4rt_acl_table_definition_obj.set_app_db_entry(
            self._p4rt_acl_table_definition_obj.TBL_NAME + ":" + table_name, attr_list
        )
        util.verify_response(
            self.response_consumer,
            self._p4rt_acl_table_definition_obj.TBL_NAME + ":" + table_name,
            attr_list,
            "SWSS_RC_SUCCESS",
        )

        # query application database for ACL tables
        acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
        )
        assert len(acl_tables) == len(original_appl_acl_tables) + 1

        # query application database for newly created ACL table
        (status, fvs) = util.get_key(
            self._p4rt_acl_table_definition_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
            table_name,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query application state database for ACL tables
        state_acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
        )
        assert len(state_acl_tables) == len(original_appl_state_acl_tables) + 1

        # query application state database for newly created ACL table
        (status, fvs) = util.get_key(
            self._p4rt_acl_table_definition_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
            table_name,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query ASIC database for default UDF wildcard match
        udf_match_asic_db_key = original_asic_udf_matches[0]

        (status, fvs) = util.get_key(
            self._p4rt_udf_match_obj.asic_db,
            self._p4rt_udf_match_obj.ASIC_DB_TBL_NAME,
            udf_match_asic_db_key,
        )
        assert status == True
        attr_list = [("NULL", "NULL")]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for UDF groups
        udf_groups_asic = util.get_keys(
            self._p4rt_udf_group_obj.asic_db, self._p4rt_udf_group_obj.ASIC_DB_TBL_NAME
        )
        assert len(udf_groups_asic) == len(original_asic_udf_groups) + 2

        # query ASIC database for newly created UDF groups
        udf_groups_asic_db_keys = [
            key for key in udf_groups_asic if key not in original_asic_udf_groups
        ]
        assert len(udf_groups_asic_db_keys) == 2
        udf_groups_asic_db_keys.sort()
        udf_group_min_asic_db_key = udf_groups_asic_db_keys[0]
        udf_group_1_asic_db_key = udf_groups_asic_db_keys[1]

        (status, fvs) = util.get_key(
            self._p4rt_udf_group_obj.asic_db,
            self._p4rt_udf_group_obj.ASIC_DB_TBL_NAME,
            udf_group_min_asic_db_key,
        )
        assert status == True
        attr_list = [
            (
                self._p4rt_udf_group_obj.SAI_UDF_GROUP_ATTR_TYPE,
                self._p4rt_udf_group_obj.SAI_UDF_GROUP_TYPE_GENERIC,
            ),
            (self._p4rt_udf_group_obj.SAI_UDF_GROUP_ATTR_LENGTH, "2"),
        ]
        util.verify_attr(fvs, attr_list)

        (status, fvs) = util.get_key(
            self._p4rt_udf_group_obj.asic_db,
            self._p4rt_udf_group_obj.ASIC_DB_TBL_NAME,
            udf_group_1_asic_db_key,
        )
        assert status == True
        attr_list = [
            (
                self._p4rt_udf_group_obj.SAI_UDF_GROUP_ATTR_TYPE,
                self._p4rt_udf_group_obj.SAI_UDF_GROUP_TYPE_GENERIC,
            ),
            (self._p4rt_udf_group_obj.SAI_UDF_GROUP_ATTR_LENGTH, "2"),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for UDFs
        udfs_asic = util.get_keys(
            self._p4rt_udf_obj.asic_db, self._p4rt_udf_obj.ASIC_DB_TBL_NAME
        )
        assert len(udfs_asic) == len(original_asic_udfs) + 2

        # query ASIC database for newly created UDFs
        udfs_asic_db_keys = [key for key in udfs_asic if key not in original_asic_udfs]
        assert len(udfs_asic_db_keys) == 2
        udfs_asic_db_keys.sort()
        udf_0_asic_db_key = udfs_asic_db_keys[0]
        udf_1_asic_db_key = udfs_asic_db_keys[1]

        (status, fvs) = util.get_key(
            self._p4rt_udf_obj.asic_db,
            self._p4rt_udf_obj.ASIC_DB_TBL_NAME,
            udf_0_asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_udf_obj.SAI_UDF_ATTR_MATCH_ID, udf_match_asic_db_key),
            (self._p4rt_udf_obj.SAI_UDF_ATTR_GROUP_ID, udf_group_min_asic_db_key),
            (self._p4rt_udf_obj.SAI_UDF_ATTR_OFFSET, "24"),
            (self._p4rt_udf_obj.SAI_UDF_ATTR_BASE, "SAI_UDF_BASE_L3"),
        ]
        util.verify_attr(fvs, attr_list)

        (status, fvs) = util.get_key(
            self._p4rt_udf_obj.asic_db,
            self._p4rt_udf_obj.ASIC_DB_TBL_NAME,
            udf_1_asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_udf_obj.SAI_UDF_ATTR_MATCH_ID, udf_match_asic_db_key),
            (self._p4rt_udf_obj.SAI_UDF_ATTR_GROUP_ID, udf_group_1_asic_db_key),
            (self._p4rt_udf_obj.SAI_UDF_ATTR_OFFSET, "26"),
            (self._p4rt_udf_obj.SAI_UDF_ATTR_BASE, "SAI_UDF_BASE_L3"),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL tables
        acl_asic_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.asic_db,
            self._p4rt_acl_table_definition_obj.ASIC_DB_TBL_NAME,
        )
        assert len(acl_asic_tables) == len(original_asic_acl_tables) + 1

        # query ASIC database for newly created ACL table
        table_asic_db_keys = [
            key for key in acl_asic_tables if key not in original_asic_acl_tables
        ]
        assert len(table_asic_db_keys) == 1
        table_asic_db_key = table_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_table_definition_obj.asic_db,
            self._p4rt_acl_table_definition_obj.ASIC_DB_TBL_NAME,
            table_asic_db_key,
        )
        assert status == True
        attr_list = [
            (
                self._p4rt_acl_table_definition_obj.SAI_ACL_TABLE_ATTR_ACL_STAGE,
                "SAI_ACL_STAGE_INGRESS",
            ),
            (self._p4rt_acl_table_definition_obj.SAI_ACL_TABLE_ATTR_SIZE, size),
            (self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_ETHER_TYPE, "true"),
            (self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_IP_TYPE, "true"),
            (self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_DST_MAC, "true"),
            (self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_SRC_IPV6_WORD3, "true"),
            (self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_SRC_IPV6_WORD2, "true"),
            (
                self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_UDF_GROUP_MIN,
                udf_group_min_asic_db_key,
            ),
            (
                self._p4rt_acl_table_definition_obj.SAI_ATTR_MATCH_UDF_GROUP_1,
                udf_group_1_asic_db_key,
            ),
            (
                self._p4rt_acl_table_definition_obj.SAI_ATTR_ACTION_TYPE_LIST,
                "1:SAI_ACL_ACTION_TYPE_COUNTER",
            ),
        ]
        util.verify_attr(fvs, attr_list)

        # maintain list of original Application and ASIC DB ACL entries before adding
        # new ACL rule
        original_appl_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        original_appl_state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        original_asic_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        original_asic_acl_counters = util.get_keys(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
        )
        original_asic_acl_meters = util.get_keys(
            self._p4rt_acl_meter_obj.asic_db, self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME
        )

        # create ACL rule 1
        rule_json_key1 = '{"match/ether_type":"0x0800","match/ether_dst":"00:1a:11:17:5f:80","match/src_ipv6_64bit":"fdf8:f53b:82e4::","match/arp_tpa":"0xff665543","priority":100}'
        action = "copy_and_set_tc"
        meter_cir = "80"
        meter_cbs = "80"
        meter_pir = "200"
        meter_pbs = "200"
        table_name_with_rule_key1 = table_name + ":" + rule_json_key1

        attr_list = [
            (self._p4rt_acl_rule_obj.ACTION, action),
            ("param/traffic_class", "1"),
            (self._p4rt_acl_rule_obj.METER_CIR, meter_cir),
            (self._p4rt_acl_rule_obj.METER_CBURST, meter_cbs),
            (self._p4rt_acl_rule_obj.METER_PIR, meter_pir),
            (self._p4rt_acl_rule_obj.METER_PBURST, meter_pbs),
        ]

        self._p4rt_acl_rule_obj.set_app_db_entry(table_name_with_rule_key1, attr_list)
        util.verify_response(
            self.response_consumer,
            table_name_with_rule_key1,
            attr_list,
            "SWSS_RC_SUCCESS",
        )

        # query application database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(acl_rules) == len(original_appl_acl_rules) + 1

        # query application database for newly created ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key1,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query application state database for ACL rules
        state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(state_acl_rules) == len(original_appl_state_acl_rules) + 1

        # query application state database for newly created ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key1,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL counters
        acl_asic_counters = util.get_keys(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
        )
        assert len(acl_asic_counters) == len(original_asic_acl_counters) + 1

        # query ASIC database for newly created ACL counter
        counter_asic_db_keys = [
            key for key in acl_asic_counters if key not in original_asic_acl_counters
        ]
        assert len(counter_asic_db_keys) == 1
        counter_asic_db_key1 = counter_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
            counter_asic_db_key1,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_counter_obj.SAI_ATTR_ENABLE_PACKET_COUNT, "true"),
            (self._p4rt_acl_counter_obj.SAI_ATTR_ENABLE_BYTE_COUNT, "true"),
            (self._p4rt_acl_counter_obj.SAI_ATTR_TABLE_ID, table_asic_db_key),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL meters
        acl_asic_meters = util.get_keys(
            self._p4rt_acl_meter_obj.asic_db, self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_meters) == len(original_asic_acl_meters) + 1

        # query ASIC database for newly created ACL meter
        meter_asic_db_keys = [
            key for key in acl_asic_meters if key not in original_asic_acl_meters
        ]
        assert len(meter_asic_db_keys) == 1
        meter_asic_db_key1 = meter_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_meter_obj.asic_db,
            self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME,
            meter_asic_db_key1,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_TYPE, "SAI_METER_TYPE_PACKETS"),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_MODE, "SAI_POLICER_MODE_TR_TCM"),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_CIR, meter_cir),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_CBS, meter_cbs),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_PIR, meter_pir),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_PBS, meter_pbs),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL rules
        acl_asic_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_rules) == len(original_asic_acl_rules) + 1

        # query ASIC database for newly created ACL rule
        rule_asic_db_keys = [
            key for key in acl_asic_rules if key not in original_asic_acl_rules
        ]
        assert len(rule_asic_db_keys) == 1
        rule_asic_db_key1 = rule_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.asic_db,
            self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME,
            rule_asic_db_key1,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_rule_obj.SAI_ATTR_ACTION_SET_TC, "1"),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_ACTION_PACKET_ACTION,
                "SAI_PACKET_ACTION_COPY",
            ),
            (self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_ETHER_TYPE, "2048&mask:0xffff"),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_IP_TYPE,
                "SAI_ACL_IP_TYPE_ANY&mask:0xffffffffffffffff",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_DST_MAC,
                "00:1A:11:17:5F:80&mask:FF:FF:FF:FF:FF:FF",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_SRC_IPV6_WORD3,
                "fdf8:f53b::&mask:ffff:ffff::",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_SRC_IPV6_WORD2,
                "0:0:82e4::&mask:0:0:ffff:ffff::",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_UDF_GROUP_MIN,
                "2:255,102&mask:2:0xff,0xff",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_UDF_GROUP_1,
                "2:85,67&mask:2:0xff,0xff",
            ),
            (self._p4rt_acl_rule_obj.SAI_ATTR_TABLE_ID, table_asic_db_key),
            (self._p4rt_acl_rule_obj.SAI_ATTR_SET_POLICER, meter_asic_db_key1),
            (self._p4rt_acl_rule_obj.SAI_ATTR_COUNTER, counter_asic_db_key1),
            (self._p4rt_acl_rule_obj.SAI_ATTR_ADMIN_STATE, "true"),
            (self._p4rt_acl_rule_obj.SAI_ATTR_PRIORITY, "100"),
        ]
        util.verify_attr(fvs, attr_list)

        # Update action and meter of rule 1 to punt_and_set_tc
        rule_json_key1 = '{"match/ether_type":"0x0800","match/ether_dst":"00:1a:11:17:5f:80","match/src_ipv6_64bit":"fdf8:f53b:82e4::","match/arp_tpa":"0xff665543","priority":100}'
        action = "punt_and_set_tc"
        meter_cir = "100"
        meter_cbs = "100"
        meter_pir = "400"
        meter_pbs = "400"
        table_name_with_rule_key1 = table_name + ":" + rule_json_key1

        attr_list = [
            (self._p4rt_acl_rule_obj.ACTION, action),
            ("param/traffic_class", "2"),
            (self._p4rt_acl_rule_obj.METER_CIR, meter_cir),
            (self._p4rt_acl_rule_obj.METER_CBURST, meter_cbs),
            (self._p4rt_acl_rule_obj.METER_PIR, meter_pir),
            (self._p4rt_acl_rule_obj.METER_PBURST, meter_pbs),
        ]

        self._p4rt_acl_rule_obj.set_app_db_entry(table_name_with_rule_key1, attr_list)
        util.verify_response(
            self.response_consumer,
            table_name_with_rule_key1,
            attr_list,
            "SWSS_RC_SUCCESS",
        )

        # query application database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(acl_rules) == len(original_appl_acl_rules) + 1

        # query application database for updated ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key1,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query application state database for ACL rules
        state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(state_acl_rules) == len(original_appl_state_acl_rules) + 1

        # query application state database for updated ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key1,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL counters
        acl_asic_counters = util.get_keys(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
        )
        assert len(acl_asic_counters) == len(original_asic_acl_counters) + 1

        # query ASIC database for the ACL counter
        counter_asic_db_keys = [
            key for key in acl_asic_counters if key not in original_asic_acl_counters
        ]
        assert len(counter_asic_db_keys) == 1
        counter_asic_db_key1 = counter_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
            counter_asic_db_key1,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_counter_obj.SAI_ATTR_ENABLE_PACKET_COUNT, "true"),
            (self._p4rt_acl_counter_obj.SAI_ATTR_ENABLE_BYTE_COUNT, "true"),
            (self._p4rt_acl_counter_obj.SAI_ATTR_TABLE_ID, table_asic_db_key),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL meters
        acl_asic_meters = util.get_keys(
            self._p4rt_acl_meter_obj.asic_db, self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_meters) == len(original_asic_acl_meters) + 1

        # query ASIC database for updated ACL meter
        meter_asic_db_keys = [
            key for key in acl_asic_meters if key not in original_asic_acl_meters
        ]
        assert len(meter_asic_db_keys) == 1
        meter_asic_db_key1 = meter_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_meter_obj.asic_db,
            self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME,
            meter_asic_db_key1,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_TYPE, "SAI_METER_TYPE_PACKETS"),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_MODE, "SAI_POLICER_MODE_TR_TCM"),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_CIR, meter_cir),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_CBS, meter_cbs),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_PIR, meter_pir),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_PBS, meter_pbs),
            (
                self._p4rt_acl_meter_obj.SAI_ATTR_RED_PACKET_ACTION,
                "SAI_PACKET_ACTION_TRAP",
            ),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL rules
        acl_asic_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_rules) == len(original_asic_acl_rules) + 1

        # query ASIC database for updated ACL rule
        rule_asic_db_keys = [
            key for key in acl_asic_rules if key not in original_asic_acl_rules
        ]
        assert len(rule_asic_db_keys) == 1
        rule_asic_db_key1 = rule_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.asic_db,
            self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME,
            rule_asic_db_key1,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_rule_obj.SAI_ATTR_ACTION_SET_TC, "2"),
            (self._p4rt_acl_rule_obj.SAI_ATTR_ACTION_PACKET_ACTION, "disabled"),
            (self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_ETHER_TYPE, "2048&mask:0xffff"),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_IP_TYPE,
                "SAI_ACL_IP_TYPE_ANY&mask:0xffffffffffffffff",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_DST_MAC,
                "00:1A:11:17:5F:80&mask:FF:FF:FF:FF:FF:FF",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_SRC_IPV6_WORD3,
                "fdf8:f53b::&mask:ffff:ffff::",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_SRC_IPV6_WORD2,
                "0:0:82e4::&mask:0:0:ffff:ffff::",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_UDF_GROUP_MIN,
                "2:255,102&mask:2:0xff,0xff",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_UDF_GROUP_1,
                "2:85,67&mask:2:0xff,0xff",
            ),
            (self._p4rt_acl_rule_obj.SAI_ATTR_TABLE_ID, table_asic_db_key),
            (self._p4rt_acl_rule_obj.SAI_ATTR_SET_POLICER, meter_asic_db_key1),
            (self._p4rt_acl_rule_obj.SAI_ATTR_COUNTER, counter_asic_db_key1),
            (self._p4rt_acl_rule_obj.SAI_ATTR_ADMIN_STATE, "true"),
            (self._p4rt_acl_rule_obj.SAI_ATTR_PRIORITY, "100"),
        ]
        util.verify_attr(fvs, attr_list)

        # create ACL rule 2 with QOS_QUEUE action
        rule_json_key2 = '{"match/is_ip":"0x1","match/ether_type":"0x0800 & 0xFFFF","match/ether_dst":"AA:BB:CC:DD:EE:FF & FF:FF:FF:FF:FF:FF","priority":100}'
        action = "qos_queue"
        meter_cir = "80"
        meter_cbs = "80"
        meter_pir = "200"
        meter_pbs = "200"
        table_name_with_rule_key2 = table_name + ":" + rule_json_key2

        attr_list = [
            (self._p4rt_acl_rule_obj.ACTION, action),
            ("param/cpu_queue", "5"),
            (self._p4rt_acl_rule_obj.METER_CIR, meter_cir),
            (self._p4rt_acl_rule_obj.METER_CBURST, meter_cbs),
            (self._p4rt_acl_rule_obj.METER_PIR, meter_pir),
            (self._p4rt_acl_rule_obj.METER_PBURST, meter_pbs),
        ]

        self._p4rt_acl_rule_obj.set_app_db_entry(table_name_with_rule_key2, attr_list)
        util.verify_response(
            self.response_consumer,
            table_name_with_rule_key2,
            attr_list,
            "SWSS_RC_SUCCESS",
        )

        # query application database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(acl_rules) == len(original_appl_acl_rules) + 2

        # query application database for newly created ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key2,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query application state database for ACL rules
        state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(state_acl_rules) == len(original_appl_state_acl_rules) + 2

        # query application state database for newly created ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key2,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL counters
        acl_asic_counters = util.get_keys(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
        )
        assert len(acl_asic_counters) == len(original_asic_acl_counters) + 2

        # query ASIC database for newly created ACL counter
        counter_asic_db_keys = [
            key
            for key in acl_asic_counters
            if key not in original_asic_acl_counters and key != counter_asic_db_key1
        ]
        assert len(counter_asic_db_keys) == 1
        counter_asic_db_key2 = counter_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
            counter_asic_db_key2,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_counter_obj.SAI_ATTR_ENABLE_PACKET_COUNT, "true"),
            (self._p4rt_acl_counter_obj.SAI_ATTR_ENABLE_BYTE_COUNT, "true"),
            (self._p4rt_acl_counter_obj.SAI_ATTR_TABLE_ID, table_asic_db_key),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for ACL meters
        acl_asic_meters = util.get_keys(
            self._p4rt_acl_meter_obj.asic_db, self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_meters) == len(original_asic_acl_meters) + 2

        # query ASIC database for newly created ACL meter
        meter_asic_db_keys = [
            key
            for key in acl_asic_meters
            if key not in original_asic_acl_meters and key != meter_asic_db_key1
        ]
        assert len(meter_asic_db_keys) == 1
        meter_asic_db_key2 = meter_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_meter_obj.asic_db,
            self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME,
            meter_asic_db_key2,
        )
        assert status == True
        attr_list = [
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_TYPE, "SAI_METER_TYPE_PACKETS"),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_MODE, "SAI_POLICER_MODE_TR_TCM"),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_CIR, meter_cir),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_CBS, meter_cbs),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_PIR, meter_pir),
            (self._p4rt_acl_meter_obj.SAI_ATTR_METER_PBS, meter_pbs),
        ]
        util.verify_attr(fvs, attr_list)

        # query ASIC database for trap groups
        trap_group_keys = util.get_keys(
            self._p4rt_trap_group_obj.asic_db,
            self._p4rt_trap_group_obj.ASIC_DB_TBL_NAME,
        )
        # default trap groups in and one trap groups per cpu queue
        # are defined in files/image_config/copp/copp_cfg.j2
        # get trap group with cpu queue num 5
        for key in trap_group_keys:
            (status, fvs) = util.get_key(
                self._p4rt_trap_group_obj.asic_db,
                self._p4rt_trap_group_obj.ASIC_DB_TBL_NAME,
                key,
            )
            assert status == True
            if fvs[0][1] == "5":
                trap_group_asic_db_key = key
                break

        # query ASIC database for user defined traps
        user_trap_keys = util.get_keys(
            self._p4rt_user_trap_obj.asic_db, self._p4rt_user_trap_obj.ASIC_DB_TBL_NAME
        )
        assert len(user_trap_keys) == 8

        # get user trap with trap group oid
        for key in user_trap_keys:
            (status, fvs) = util.get_key(
                self._p4rt_user_trap_obj.asic_db,
                self._p4rt_user_trap_obj.ASIC_DB_TBL_NAME,
                key,
            )
            assert status == True
            if (
                fvs[0][1] == trap_group_asic_db_key
                or fvs[1][1] == trap_group_asic_db_key
            ):
                user_trap_asic_db_key = key
                break

        # query ASIC database for ACL rules
        acl_asic_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_rules) == len(original_asic_acl_rules) + 2

        # query ASIC database for newly created ACL rule
        rule_asic_db_keys = [
            key
            for key in acl_asic_rules
            if key not in original_asic_acl_rules and key != rule_asic_db_key1
        ]
        assert len(rule_asic_db_keys) == 1
        rule_asic_db_key2 = rule_asic_db_keys[0]

        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.asic_db,
            self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME,
            rule_asic_db_key2,
        )
        assert status == True
        attr_list = [
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_ACTION_SET_USER_TRAP_ID,
                user_trap_asic_db_key,
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_ACTION_PACKET_ACTION,
                "SAI_PACKET_ACTION_TRAP",
            ),
            (self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_ETHER_TYPE, "2048&mask:0xffff"),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_IP_TYPE,
                "SAI_ACL_IP_TYPE_IP&mask:0xffffffffffffffff",
            ),
            (
                self._p4rt_acl_rule_obj.SAI_ATTR_MATCH_DST_MAC,
                "AA:BB:CC:DD:EE:FF&mask:FF:FF:FF:FF:FF:FF",
            ),
            (self._p4rt_acl_rule_obj.SAI_ATTR_TABLE_ID, table_asic_db_key),
            (self._p4rt_acl_rule_obj.SAI_ATTR_SET_POLICER, meter_asic_db_key2),
            (self._p4rt_acl_rule_obj.SAI_ATTR_COUNTER, counter_asic_db_key2),
            (self._p4rt_acl_rule_obj.SAI_ATTR_ADMIN_STATE, "true"),
            (self._p4rt_acl_rule_obj.SAI_ATTR_PRIORITY, "100"),
        ]
        util.verify_attr(fvs, attr_list)

        # remove ACL rule 1
        self._p4rt_acl_rule_obj.remove_app_db_entry(table_name_with_rule_key1)
        util.verify_response(
            self.response_consumer, table_name_with_rule_key1, [], "SWSS_RC_SUCCESS"
        )

        # query application database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(acl_rules) == len(original_appl_acl_rules) + 1

        # verify that the ACL rule no longer exists in application database
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key1,
        )
        assert status == False

        # query application state database for ACL rules
        state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(state_acl_rules) == len(original_appl_state_acl_rules) + 1

        # verify that the ACL rule no longer exists in application state database
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key1,
        )
        assert status == False

        # query ASIC database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_rules) == len(original_asic_acl_rules) + 1

        # verify that removed ACL rule no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.asic_db,
            self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME,
            rule_asic_db_key1,
        )
        assert status == False

        # verify that removed ACL counter no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
            counter_asic_db_key1,
        )
        assert status == False

        # verify that removed ACL meter no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_meter_obj.asic_db,
            self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME,
            meter_asic_db_key1,
        )
        assert status == False

        # remove ACL rule 2
        self._p4rt_acl_rule_obj.remove_app_db_entry(table_name_with_rule_key2)
        util.verify_response(
            self.response_consumer, table_name_with_rule_key2, [], "SWSS_RC_SUCCESS"
        )

        # query application database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(acl_rules) == len(original_appl_acl_rules)

        # verify that the ACL rule no longer exists in application database
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key2,
        )
        assert status == False

        # query application state database for ACL rules
        state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(state_acl_rules) == len(original_appl_state_acl_rules)

        # verify that the ACL rule no longer exists in application state database
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key2,
        )
        assert status == False

        # query ASIC database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_rules) == len(original_asic_acl_rules)

        # verify that removed ACL rule no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.asic_db,
            self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME,
            rule_asic_db_key2,
        )
        assert status == False

        # verify that removed ACL counter no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_counter_obj.asic_db,
            self._p4rt_acl_counter_obj.ASIC_DB_TBL_NAME,
            counter_asic_db_key2,
        )
        assert status == False

        # verify that removed ACL meter no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_meter_obj.asic_db,
            self._p4rt_acl_meter_obj.ASIC_DB_TBL_NAME,
            meter_asic_db_key2,
        )
        assert status == False

        # remove ACL table
        self._p4rt_acl_table_definition_obj.remove_app_db_entry(
            self._p4rt_acl_table_definition_obj.TBL_NAME + ":" + table_name
        )
        util.verify_response(
            self.response_consumer,
            self._p4rt_acl_table_definition_obj.TBL_NAME + ":" + table_name,
            [],
            "SWSS_RC_SUCCESS",
        )

        # query application database for ACL tables
        acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
        )
        assert len(acl_tables) == len(original_appl_acl_tables)

        # verify that the ACL table no longer exists in application database
        (status, fvs) = util.get_key(
            self._p4rt_acl_table_definition_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            self._p4rt_acl_table_definition_obj.TBL_NAME + ":" + table_name,
        )
        assert status == False

        # query application state database for ACL tables
        state_acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_acl_table_definition_obj.TBL_NAME,
        )
        assert len(state_acl_tables) == len(original_appl_state_acl_tables)

        # verify that the ACL table no longer exists in application state database
        (status, fvs) = util.get_key(
            self._p4rt_acl_table_definition_obj.appl_state_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            self._p4rt_acl_table_definition_obj.TBL_NAME + ":" + table_name,
        )
        assert status == False

        # query ASIC database for ACL tables
        acl_tables = util.get_keys(
            self._p4rt_acl_table_definition_obj.asic_db,
            self._p4rt_acl_table_definition_obj.ASIC_DB_TBL_NAME,
        )
        assert len(acl_tables) == len(original_asic_acl_tables)

        # verify that removed ACL table no longer exists in ASIC database
        (status, fvs) = util.get_key(
            self._p4rt_acl_table_definition_obj.asic_db,
            self._p4rt_acl_table_definition_obj.ASIC_DB_TBL_NAME,
            table_asic_db_key,
        )
        assert status == False

    def test_AclRuleAddWithoutTableDefinitionFails(self, dvs, testlog):
        # initialize ACL table objects and database connectors
        self._set_up(dvs)

        # maintain list of original Application and ASIC DB ACL entries before adding
        # new ACL rule
        table_name = "ACL_PUNT_TABLE_RULE_TEST"
        original_appl_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        original_appl_state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        original_asic_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )

        # create ACL rule
        rule_json_key = '{"match/ether_type":"0x0800","match/ether_dst":"00:1a:11:17:5f:80","match/src_ipv6_64bit":"fdf8:f53b:82e4::","match/arp_tpa":"0xff665543","priority":100}'
        action = "copy_and_set_tc"
        meter_cir = "80"
        meter_cbs = "80"
        meter_pir = "200"
        meter_pbs = "200"
        table_name_with_rule_key = table_name + ":" + rule_json_key

        attr_list = [
            (self._p4rt_acl_rule_obj.ACTION, action),
            ("param/traffic_class", "1"),
            (self._p4rt_acl_rule_obj.METER_CIR, meter_cir),
            (self._p4rt_acl_rule_obj.METER_CBURST, meter_cbs),
            (self._p4rt_acl_rule_obj.METER_PIR, meter_pir),
            (self._p4rt_acl_rule_obj.METER_PBURST, meter_pbs),
        ]

        self._p4rt_acl_rule_obj.set_app_db_entry(table_name_with_rule_key, attr_list)
        util.verify_response(
            self.response_consumer,
            table_name_with_rule_key,
            attr_list,
            "SWSS_RC_INVALID_PARAM",
            "[OrchAgent] Failed to find P4Orch Manager for ACL_PUNT_TABLE_RULE_TEST P4RT DB table",
        )

        # query application database for ACL rules
        acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(acl_rules) == len(original_appl_acl_rules) + 1

        # query application database for newly created ACL rule
        (status, fvs) = util.get_key(
            self._p4rt_acl_rule_obj.appl_db,
            self._p4rt_acl_table_definition_obj.APP_DB_TBL_NAME,
            table_name_with_rule_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # query application state database for ACL rules
        state_acl_rules = util.get_keys(
            self._p4rt_acl_rule_obj.appl_state_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME + ":" + table_name,
        )
        assert len(state_acl_rules) == len(original_appl_state_acl_rules)

        # query ASIC database for ACL rules
        acl_asic_rules = util.get_keys(
            self._p4rt_acl_rule_obj.asic_db, self._p4rt_acl_rule_obj.ASIC_DB_TBL_NAME
        )
        assert len(acl_asic_rules) == len(original_asic_acl_rules)

        # query ASIC database for newly created ACL rule
        rule_asic_db_keys = [
            key for key in acl_asic_rules if key not in original_asic_acl_rules
        ]
        assert len(rule_asic_db_keys) == 0

        # cleanup application database
        tbl = swsscommon.Table(
            self._p4rt_acl_table_definition_obj.appl_db,
            self._p4rt_acl_rule_obj.APP_DB_TBL_NAME,
        )
        tbl._del(table_name_with_rule_key)
