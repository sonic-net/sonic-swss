import pytest
import logging

import test_flex_counters as flex_counter_module


PBH_HASH_FIELD_NAME = "inner_ip_proto"
PBH_HASH_FIELD_HASH_FIELD = "INNER_IP_PROTOCOL"
PBH_HASH_FIELD_SEQUENCE_ID = "1"

PBH_HASH_NAME = "inner_v4_hash"
PBH_HASH_HASH_FIELD_LIST = ["inner_ip_proto"]

PBH_RULE_NAME = "nvgre"
PBH_RULE_PRIORITY = "1"
PBH_RULE_ETHER_TYPE = "0x0800"
PBH_RULE_IP_PROTOCOL = "0x2f"
PBH_RULE_GRE_KEY = "0x2500/0xffffff00"
PBH_RULE_INNER_ETHER_TYPE = "0x86dd"
PBH_RULE_HASH = "inner_v4_hash"

PBH_TABLE_NAME = "pbh_table"
PBH_TABLE_INTERFACE_LIST = ["Ethernet0", "Ethernet4"]
PBH_TABLE_DESCRIPTION = "NVGRE and VxLAN"


logging.basicConfig(level=logging.INFO)
pbhlogger = logging.getLogger(__name__)


@pytest.fixture(autouse=True, scope="class")
def dvs_api(request, dvs_pbh, dvs_acl):
    # Fixtures are created when first requested by a test, and are destroyed based on their scope
    if request.cls is None:
        yield
        return
    pbhlogger.info("Initialize DVS API: PBH, ACL")
    request.cls.dvs_pbh = dvs_pbh
    request.cls.dvs_acl = dvs_acl
    yield
    pbhlogger.info("Deinitialize DVS API: PBH, ACL")
    del request.cls.dvs_pbh
    del request.cls.dvs_acl


@pytest.mark.usefixtures("dvs_lag_manager")
class TestPbhInterfaceBinding:
    def test_PbhTablePortBinding(self, testlog):
        try:
            port_list = ["Ethernet0", "Ethernet4"]

            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=port_list,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)

            pbhlogger.info("Validate PBH table port binding: {}".format(",".join(port_list)))
            acl_table_id = self.dvs_acl.get_acl_table_ids(1)[0]
            acl_table_group_ids = self.dvs_acl.get_acl_table_group_ids(len(port_list))

            self.dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            self.dvs_acl.verify_acl_table_port_binding(acl_table_id, port_list, 1)
        finally:
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(0)

    def test_PbhTablePortChannelBinding(self, testlog):
        try:
            # PortChannel0001
            pbhlogger.info("Create LAG: PortChannel0001")
            self.dvs_lag.create_port_channel("0001")
            self.dvs_lag.get_and_verify_port_channel(1)

            pbhlogger.info("Create LAG member: Ethernet120")
            self.dvs_lag.create_port_channel_member("0001", "Ethernet120")
            self.dvs_lag.get_and_verify_port_channel_members(1)

            # PortChannel0002
            pbhlogger.info("Create LAG: PortChannel0002")
            self.dvs_lag.create_port_channel("0002")
            self.dvs_lag.get_and_verify_port_channel(2)

            pbhlogger.info("Create LAG member: Ethernet124")
            self.dvs_lag.create_port_channel_member("0002", "Ethernet124")
            self.dvs_lag.get_and_verify_port_channel_members(2)

            # PBH table
            portchannel_list = ["PortChannel0001", "PortChannel0002"]

            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=portchannel_list,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)

            pbhlogger.info("Validate PBH table LAG binding: {}".format(",".join(portchannel_list)))
            acl_table_id = self.dvs_acl.get_acl_table_ids(1)[0]
            acl_table_group_ids = self.dvs_acl.get_acl_table_group_ids(len(portchannel_list))

            self.dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            self.dvs_acl.verify_acl_table_portchannel_binding(acl_table_id, portchannel_list, 1)
        finally:
            # PBH table
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(0)

            # PortChannel0001
            pbhlogger.info("Remove LAG member: Ethernet120")
            self.dvs_lag.remove_port_channel_member("0001", "Ethernet120")
            self.dvs_lag.get_and_verify_port_channel_members(1)

            pbhlogger.info("Remove LAG: PortChannel0001")
            self.dvs_lag.remove_port_channel("0001")
            self.dvs_lag.get_and_verify_port_channel(1)

            # PortChannel0002
            pbhlogger.info("Remove LAG member: Ethernet124")
            self.dvs_lag.remove_port_channel_member("0002", "Ethernet124")
            self.dvs_lag.get_and_verify_port_channel_members(0)

            pbhlogger.info("Remove LAG: PortChannel0002")
            self.dvs_lag.remove_port_channel("0002")
            self.dvs_lag.get_and_verify_port_channel(0)


class TestPbhBasicFlows:
    def test_PbhHashFieldCreationDeletion(self, testlog):
        try:
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)
        finally:
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(0)

    def test_PbhHashCreationDeletion(self, testlog):
        try:
            # PBH hash field
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)

            # PBH hash
            pbhlogger.info("Create PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.create_pbh_hash(
                hash_name=PBH_HASH_NAME,
                hash_field_list=PBH_HASH_HASH_FIELD_LIST
            )
            self.dvs_pbh.verify_pbh_hash_count(1)
        finally:
            # PBH hash
            pbhlogger.info("Remove PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.remove_pbh_hash(PBH_HASH_NAME)
            self.dvs_pbh.verify_pbh_hash_count(0)

            # PBH hash field
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(0)

    def test_PbhTableCreationDeletion(self, testlog):
        try:
            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=PBH_TABLE_INTERFACE_LIST,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)
        finally:
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(0)

    def test_PbhRuleCreationDeletion(self, testlog):
        try:
            # PBH hash field
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)

            # PBH hash
            pbhlogger.info("Create PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.create_pbh_hash(
                hash_name=PBH_HASH_NAME,
                hash_field_list=PBH_HASH_HASH_FIELD_LIST
            )
            self.dvs_pbh.verify_pbh_hash_count(1)

            # PBH table
            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=PBH_TABLE_INTERFACE_LIST,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)

            # PBH rule
            attr_dict = {
                "ether_type": PBH_RULE_ETHER_TYPE,
                "ip_protocol": PBH_RULE_IP_PROTOCOL,
                "gre_key": PBH_RULE_GRE_KEY,
                "inner_ether_type": PBH_RULE_INNER_ETHER_TYPE
            }

            pbhlogger.info("Create PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.create_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority=PBH_RULE_PRIORITY,
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH
            )
            self.dvs_acl.verify_acl_rule_count(1)
        finally:
            # PBH rule
            pbhlogger.info("Remove PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.remove_pbh_rule(PBH_TABLE_NAME, PBH_RULE_NAME)
            self.dvs_acl.verify_acl_rule_count(0)

            # PBH table
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(0)

            # PBH hash
            pbhlogger.info("Remove PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.remove_pbh_hash(PBH_HASH_NAME)
            self.dvs_pbh.verify_pbh_hash_count(0)

            # PBH hash field
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(0)


class TestPbhBasicEditFlows:
    def test_PbhRuleUpdate(self, testlog):
        try:
            # PBH hash field
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)

            # PBH hash
            pbhlogger.info("Create PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.create_pbh_hash(
                hash_name=PBH_HASH_NAME,
                hash_field_list=PBH_HASH_HASH_FIELD_LIST
            )
            self.dvs_pbh.verify_pbh_hash_count(1)

            # PBH table
            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=PBH_TABLE_INTERFACE_LIST,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)

            # PBH rule
            attr_dict = {
                "ether_type": PBH_RULE_ETHER_TYPE,
                "ip_protocol": PBH_RULE_IP_PROTOCOL,
                "gre_key": PBH_RULE_GRE_KEY,
                "inner_ether_type": PBH_RULE_INNER_ETHER_TYPE
            }

            pbhlogger.info("Create PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.create_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority=PBH_RULE_PRIORITY,
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH
            )
            self.dvs_acl.verify_acl_rule_count(1)

            attr_dict = {
                "ether_type": "0x86dd",
                "ipv6_next_header": "0x2f",
                "inner_ether_type": "0x0800"
            }

            pbhlogger.info("Update PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.update_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority="100",
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH,
                packet_action="SET_LAG_HASH",
                flow_counter="ENABLED"
            )

            hash_id = self.dvs_pbh.get_pbh_hash_ids(1)[0]
            counter_id = self.dvs_acl.get_acl_counter_ids(1)[0]

            sai_attr_dict = {
                "SAI_ACL_ENTRY_ATTR_PRIORITY": self.dvs_acl.get_simple_qualifier_comparator("100"),
                "SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE": self.dvs_acl.get_simple_qualifier_comparator("34525&mask:0xffff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL": self.dvs_acl.get_simple_qualifier_comparator("disabled"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER": self.dvs_acl.get_simple_qualifier_comparator("47&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_GRE_KEY": self.dvs_acl.get_simple_qualifier_comparator("disabled"),
                "SAI_ACL_ENTRY_ATTR_FIELD_INNER_ETHER_TYPE": self.dvs_acl.get_simple_qualifier_comparator("2048&mask:0xffff"),
                "SAI_ACL_ENTRY_ATTR_ACTION_SET_ECMP_HASH_ID": self.dvs_acl.get_simple_qualifier_comparator("disabled"),
                "SAI_ACL_ENTRY_ATTR_ACTION_SET_LAG_HASH_ID": self.dvs_acl.get_simple_qualifier_comparator(hash_id),
                "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER": self.dvs_acl.get_simple_qualifier_comparator(counter_id)
            }

            self.dvs_acl.verify_acl_rule_generic(
                sai_qualifiers=sai_attr_dict
            )

        finally:
            # PBH rule
            pbhlogger.info("Remove PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.remove_pbh_rule(PBH_TABLE_NAME, PBH_RULE_NAME)
            self.dvs_acl.verify_acl_rule_count(0)

            # PBH table
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(0)

            # PBH hash
            pbhlogger.info("Remove PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.remove_pbh_hash(PBH_HASH_NAME)
            self.dvs_pbh.verify_pbh_hash_count(0)

            # PBH hash field
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(0)


    def test_PbhRuleUpdateFlowCounter(self, dvs, testlog):
        try:
            # PBH hash field
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)

            # PBH hash
            pbhlogger.info("Create PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.create_pbh_hash(
                hash_name=PBH_HASH_NAME,
                hash_field_list=PBH_HASH_HASH_FIELD_LIST
            )
            self.dvs_pbh.verify_pbh_hash_count(1)

            # PBH table
            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=PBH_TABLE_INTERFACE_LIST,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)

            # Prepare ACL FLEX Counter environment
            meta_data = flex_counter_module.counter_group_meta['acl_counter']
            counter_key = meta_data['key']
            counter_stat = meta_data['group_name']
            counter_map = meta_data['name_map']

            test_flex_counters = flex_counter_module.TestFlexCounters()
            test_flex_counters.setup_dbs(dvs)
            test_flex_counters.verify_no_flex_counters_tables(counter_stat)

            # PBH rule
            pbhlogger.info("Create PBH rule: {}".format(PBH_RULE_NAME))

            attr_dict = {
                "ether_type": PBH_RULE_ETHER_TYPE,
                "ip_protocol": PBH_RULE_IP_PROTOCOL,
                "gre_key": PBH_RULE_GRE_KEY,
                "inner_ether_type": PBH_RULE_INNER_ETHER_TYPE
            }

            self.dvs_pbh.create_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority=PBH_RULE_PRIORITY,
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH,
                packet_action="SET_ECMP_HASH",
                flow_counter="ENABLED"
            )
            self.dvs_acl.verify_acl_rule_count(1)
            self.dvs_acl.get_acl_counter_ids(1)

            pbhlogger.info("Enable a ACL FLEX counter")
            test_flex_counters.set_flex_counter_group_status(counter_key, counter_map)
            test_flex_counters.set_flex_counter_group_interval(counter_key, counter_stat, '1000')
            test_flex_counters.verify_flex_counters_populated(counter_map, counter_stat)

            pbhlogger.info("Disable a flow counter for PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.update_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority=PBH_RULE_PRIORITY,
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH,
                packet_action="SET_ECMP_HASH",
                flow_counter="DISABLED"
            )
            self.dvs_acl.get_acl_counter_ids(0)

            pbhlogger.info("Enable a flow counter for PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.update_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority=PBH_RULE_PRIORITY,
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH,
                packet_action="SET_ECMP_HASH",
                flow_counter="ENABLED"
            )
            self.dvs_acl.get_acl_counter_ids(1)

        finally:
            # PBH rule
            pbhlogger.info("Remove PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.remove_pbh_rule(PBH_TABLE_NAME, PBH_RULE_NAME)
            self.dvs_acl.verify_acl_rule_count(0)

            # PBH table
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(0)

            # PBH hash
            pbhlogger.info("Remove PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.remove_pbh_hash(PBH_HASH_NAME)
            self.dvs_pbh.verify_pbh_hash_count(0)

            # PBH hash field
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(0)

            # ACL FLEX counter
            pbhlogger.info("Disable ACL FLEX counter")
            test_flex_counters.post_trap_flow_counter_test(meta_data)


@pytest.mark.usefixtures("dvs_lag_manager")
class TestPbhExtendedFlows:
    class PbhRefCountHelper(object):
        def __init__(self):
            self.hashFieldCount = 0
            self.hashCount = 0
            self.ruleCount = 0
            self.tableCount = 0

        def incPbhHashFieldCount(self):
            self.hashFieldCount += 1

        def decPbhHashFieldCount(self):
            self.hashFieldCount -= 1

        def getPbhHashFieldCount(self):
            return self.hashFieldCount

        def incPbhHashCount(self):
            self.hashCount += 1

        def decPbhHashCount(self):
            self.hashCount -= 1

        def getPbhHashCount(self):
            return self.hashCount

        def incPbhRuleCount(self):
            self.ruleCount += 1

        def decPbhRuleCount(self):
            self.ruleCount -= 1

        def getPbhRuleCount(self):
            return self.ruleCount

        def incPbhTableCount(self):
            self.tableCount += 1

        def decPbhTableCount(self):
            self.tableCount -= 1

        def getPbhTableCount(self):
            return self.tableCount

    class LagRefCountHelper(object):
        def __init__(self):
            self.lagCount = 0
            self.lagMemberCount = 0

        def incLagCount(self):
            self.lagCount += 1

        def decLagCount(self):
            self.lagCount -= 1

        def getLagCount(self):
            return self.lagCount

        def incLagMemberCount(self):
            self.lagMemberCount += 1

        def decLagMemberCount(self):
            self.lagMemberCount -= 1

        def getLagMemberCount(self):
            return self.lagMemberCount

    def strip_prefix(self, s, p):
        return s[len(p):] if s.startswith(p) else s

    def create_basic_lag(self, meta_dict, lag_ref_count):
        lag_id = self.strip_prefix(meta_dict["name"], "PortChannel")

        pbhlogger.info("Create LAG: {}".format(meta_dict["name"]))
        self.dvs_lag.create_port_channel(lag_id)
        lag_ref_count.incLagCount()
        self.dvs_lag.get_and_verify_port_channel(lag_ref_count.getLagCount())

        pbhlogger.info("Create LAG member: {}".format(meta_dict["member"]))
        self.dvs_lag.create_port_channel_member(lag_id, meta_dict["member"])
        lag_ref_count.incLagMemberCount()
        self.dvs_lag.get_and_verify_port_channel_members(lag_ref_count.getLagMemberCount())

    def remove_basic_lag(self, meta_dict, lag_ref_count):
        lag_id = self.strip_prefix(meta_dict["name"], "PortChannel")

        pbhlogger.info("Remove LAG member: {}".format(meta_dict["member"]))
        self.dvs_lag.remove_port_channel_member(lag_id, meta_dict["member"])
        lag_ref_count.decLagMemberCount()
        self.dvs_lag.get_and_verify_port_channel_members(lag_ref_count.getLagMemberCount())

        pbhlogger.info("Remove LAG: {}".format(meta_dict["name"]))
        self.dvs_lag.remove_port_channel(lag_id)
        lag_ref_count.decLagCount()
        self.dvs_lag.get_and_verify_port_channel(lag_ref_count.getLagCount())

    def create_hash_field(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Create PBH hash field: {}".format(meta_dict["name"]))
        self.dvs_pbh.create_pbh_hash_field(
            hash_field_name=meta_dict["name"],
            hash_field=meta_dict["hash_field"],
            ip_mask=meta_dict["ip_mask"] if "ip_mask" in meta_dict else None,
            sequence_id=meta_dict["sequence_id"]
        )
        pbh_ref_count.incPbhHashFieldCount()
        self.dvs_pbh.verify_pbh_hash_field_count(pbh_ref_count.getPbhHashFieldCount())

    def remove_hash_field(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Remove PBH hash field: {}".format(meta_dict["name"]))
        self.dvs_pbh.remove_pbh_hash_field(meta_dict["name"])
        pbh_ref_count.decPbhHashFieldCount()
        self.dvs_pbh.verify_pbh_hash_field_count(pbh_ref_count.getPbhHashFieldCount())

    def create_hash(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Create PBH hash: {}".format(meta_dict["name"]))
        self.dvs_pbh.create_pbh_hash(
            hash_name=meta_dict["name"],
            hash_field_list=meta_dict["hash_field_list"]
        )
        pbh_ref_count.incPbhHashCount()
        self.dvs_pbh.verify_pbh_hash_count(pbh_ref_count.getPbhHashCount())

    def remove_hash(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Remove PBH hash: {}".format(meta_dict["name"]))
        self.dvs_pbh.remove_pbh_hash(meta_dict["name"])
        pbh_ref_count.decPbhHashCount()
        self.dvs_pbh.verify_pbh_hash_count(pbh_ref_count.getPbhHashCount())

    def create_table(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Create PBH table: {}".format(meta_dict["name"]))
        self.dvs_pbh.create_pbh_table(
            table_name=meta_dict["name"],
            interface_list=meta_dict["interface_list"],
            description=meta_dict["description"]
        )
        pbh_ref_count.incPbhTableCount()
        self.dvs_acl.verify_acl_table_count(pbh_ref_count.getPbhTableCount())

    def remove_table(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Remove PBH table: {}".format(meta_dict["name"]))
        self.dvs_pbh.remove_pbh_table(meta_dict["name"])
        pbh_ref_count.decPbhTableCount()
        self.dvs_acl.verify_acl_table_count(pbh_ref_count.getPbhTableCount())

    def create_rule(self, meta_dict, attr_dict, pbh_ref_count):
        pbhlogger.info("Create PBH rule: {}".format(meta_dict["name"]))
        self.dvs_pbh.create_pbh_rule(
            table_name=meta_dict["table"],
            rule_name=meta_dict["name"],
            priority=meta_dict["priority"],
            qualifiers=attr_dict,
            hash_name=meta_dict["hash"],
            packet_action=meta_dict["packet_action"] if "packet_action" in meta_dict else None,
            flow_counter=meta_dict["flow_counter"] if "flow_counter" in meta_dict else None
        )
        pbh_ref_count.incPbhRuleCount()
        self.dvs_acl.verify_acl_rule_count(pbh_ref_count.getPbhRuleCount())

    def remove_rule(self, meta_dict, pbh_ref_count):
        pbhlogger.info("Remove PBH rule: {}".format(meta_dict["name"]))
        self.dvs_pbh.remove_pbh_rule(meta_dict["table"], meta_dict["name"])
        pbh_ref_count.decPbhRuleCount()
        self.dvs_acl.verify_acl_rule_count(pbh_ref_count.getPbhRuleCount())

    @pytest.fixture(autouse=True)
    def pbh_ref_count(self):
        pbhlogger.info("Create PBH reference count helper")
        yield self.PbhRefCountHelper()
        pbhlogger.info("Remove PBH reference count helper")

    @pytest.fixture(autouse=True)
    def lag_ref_count(self):
        pbhlogger.info("Create LAG reference count helper")
        yield self.LagRefCountHelper()
        pbhlogger.info("Remove LAG reference count helper")

    @pytest.fixture(autouse=True)
    def pbh_port_channel_0001(self, lag_ref_count):
        try:
            meta_dict = {
                "name": "PortChannel0001",
                "member": "Ethernet120"
            }
            self.create_basic_lag(meta_dict, lag_ref_count)
            yield meta_dict
        finally:
            self.remove_basic_lag(meta_dict, lag_ref_count)

    @pytest.fixture(autouse=True)
    def pbh_port_channel_0002(self, lag_ref_count):
        try:
            meta_dict = {
                "name": "PortChannel0002",
                "member": "Ethernet124"
            }
            self.create_basic_lag(meta_dict, lag_ref_count)
            yield meta_dict
        finally:
            self.remove_basic_lag(meta_dict, lag_ref_count)

    @pytest.fixture
    def pbh_inner_ip_proto(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_ip_proto",
                "hash_field": "INNER_IP_PROTOCOL",
                "sequence_id": "1"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_l4_dst_port(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_l4_dst_port",
                "hash_field": "INNER_L4_DST_PORT",
                "sequence_id": "2"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_l4_src_port(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_l4_src_port",
                "hash_field": "INNER_L4_SRC_PORT",
                "sequence_id": "2"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_dst_ipv4(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_dst_ipv4",
                "hash_field": "INNER_DST_IPV4",
                "ip_mask": "255.0.0.0",
                "sequence_id": "3"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_src_ipv4(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_src_ipv4",
                "hash_field": "INNER_SRC_IPV4",
                "ip_mask": "0.0.0.255",
                "sequence_id": "3"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_dst_ipv6(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_dst_ipv6",
                "hash_field": "INNER_DST_IPV6",
                "ip_mask": "ffff::",
                "sequence_id": "4"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_src_ipv6(self, pbh_ref_count):
        try:
            meta_dict = {
                "name": "inner_src_ipv6",
                "hash_field": "INNER_SRC_IPV6",
                "ip_mask": "::ffff",
                "sequence_id": "4"
            }
            self.create_hash_field(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash_field(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_v4(
        self,
        pbh_ref_count,
        pbh_inner_ip_proto,
        pbh_inner_l4_dst_port,
        pbh_inner_l4_src_port,
        pbh_inner_dst_ipv4,
        pbh_inner_src_ipv4
    ):
        try:
            meta_dict = {
                "name": "inner_v4_hash",
                "hash_field_list": [
                    pbh_inner_ip_proto["name"],
                    pbh_inner_l4_dst_port["name"],
                    pbh_inner_l4_src_port["name"],
                    pbh_inner_dst_ipv4["name"],
                    pbh_inner_src_ipv4["name"]
                ]
            }
            self.create_hash(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_inner_v6(
        self,
        pbh_ref_count,
        pbh_inner_ip_proto,
        pbh_inner_l4_dst_port,
        pbh_inner_l4_src_port,
        pbh_inner_dst_ipv6,
        pbh_inner_src_ipv6
    ):
        try:
            meta_dict = {
                "name": "inner_v6_hash",
                "hash_field_list": [
                    pbh_inner_ip_proto["name"],
                    pbh_inner_l4_dst_port["name"],
                    pbh_inner_l4_src_port["name"],
                    pbh_inner_dst_ipv6["name"],
                    pbh_inner_src_ipv6["name"]
                ]
            }
            self.create_hash(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_hash(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_table(
        self,
        pbh_ref_count,
        pbh_port_channel_0001,
        pbh_port_channel_0002
    ):
        try:
            meta_dict = {
                "name": "pbh_table",
                "interface_list": [
                    "Ethernet0",
                    "Ethernet4",
                    pbh_port_channel_0001["name"],
                    pbh_port_channel_0002["name"]
                ],
                "description": "NVGRE and VxLAN"
            }
            self.create_table(meta_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_table(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_nvgre(
        self,
        pbh_ref_count,
        pbh_table,
        pbh_inner_v6
    ):
        try:
            meta_dict = {
                "table": pbh_table["name"],
                "name": "nvgre",
                "priority": "1",
                "ether_type": "0x0800",
                "ip_protocol": "0x2f",
                "gre_key": "0x2500/0xffffff00",
                "inner_ether_type": "0x86dd",
                "hash": pbh_inner_v6["name"],
                "packet_action": "SET_ECMP_HASH",
                "flow_counter": "DISABLED"
            }
            attr_dict = {
                "gre_key": meta_dict["gre_key"],
                "inner_ether_type": meta_dict["inner_ether_type"]
            }
            self.create_rule(meta_dict, attr_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_rule(meta_dict, pbh_ref_count)

    @pytest.fixture
    def pbh_vxlan(
        self,
        pbh_ref_count,
        pbh_table,
        pbh_inner_v4
    ):
        try:
            meta_dict = {
                "table": pbh_table["name"],
                "name": "vxlan",
                "priority": "2",
                "ether_type": "0x0800",
                "ip_protocol": "0x11",
                "l4_dst_port": "0x12b5",
                "inner_ether_type": "0x0800",
                "hash": pbh_inner_v4["name"],
                "packet_action": "SET_LAG_HASH",
                "flow_counter": "ENABLED"
            }
            attr_dict = {
                "ip_protocol": meta_dict["ip_protocol"],
                "l4_dst_port": meta_dict["l4_dst_port"],
                "inner_ether_type": meta_dict["inner_ether_type"]
            }
            self.create_rule(meta_dict, attr_dict, pbh_ref_count)
            yield meta_dict
        finally:
            self.remove_rule(meta_dict, pbh_ref_count)

    def test_PbhNvgreVxlanConfiguration(self, testlog, pbh_nvgre, pbh_vxlan):
        pass


class TestPbhDependencyFlows:
    def test_PbhHashCreationDeletionWithDependencies(self, testlog):
        try:
            # PBH hash
            pbhlogger.info("Create PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.create_pbh_hash(
                hash_name=PBH_HASH_NAME,
                hash_field_list=PBH_HASH_HASH_FIELD_LIST
            )
            self.dvs_pbh.verify_pbh_hash_count(0)

            # PBH hash field
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)
            self.dvs_pbh.verify_pbh_hash_count(1)
        finally:
            # PBH hash field
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(1)

            # PBH hash
            pbhlogger.info("Remove PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.remove_pbh_hash(PBH_HASH_NAME)
            self.dvs_pbh.verify_pbh_hash_count(0)
            self.dvs_pbh.verify_pbh_hash_field_count(0)

    def test_PbhRuleCreationDeletionWithDependencies(self, testlog):
        try:
            # PBH hash
            pbhlogger.info("Create PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.create_pbh_hash(
                hash_name=PBH_HASH_NAME,
                hash_field_list=PBH_HASH_HASH_FIELD_LIST
            )
            self.dvs_pbh.verify_pbh_hash_count(0)

            # PBH hash field
            pbhlogger.info("Create PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.create_pbh_hash_field(
                hash_field_name=PBH_HASH_FIELD_NAME,
                hash_field=PBH_HASH_FIELD_HASH_FIELD,
                sequence_id=PBH_HASH_FIELD_SEQUENCE_ID
            )
            self.dvs_pbh.verify_pbh_hash_field_count(1)
            self.dvs_pbh.verify_pbh_hash_count(1)

            # PBH rule
            attr_dict = {
                "ether_type": PBH_RULE_ETHER_TYPE,
                "ip_protocol": PBH_RULE_IP_PROTOCOL,
                "gre_key": PBH_RULE_GRE_KEY,
                "inner_ether_type": PBH_RULE_INNER_ETHER_TYPE
            }

            pbhlogger.info("Create PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.create_pbh_rule(
                table_name=PBH_TABLE_NAME,
                rule_name=PBH_RULE_NAME,
                priority=PBH_RULE_PRIORITY,
                qualifiers=attr_dict,
                hash_name=PBH_RULE_HASH
            )
            self.dvs_acl.verify_acl_rule_count(0)

            # PBH table
            pbhlogger.info("Create PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.create_pbh_table(
                table_name=PBH_TABLE_NAME,
                interface_list=PBH_TABLE_INTERFACE_LIST,
                description=PBH_TABLE_DESCRIPTION
            )
            self.dvs_acl.verify_acl_table_count(1)
            self.dvs_acl.verify_acl_rule_count(1)

        finally:
            # PBH table
            pbhlogger.info("Remove PBH table: {}".format(PBH_TABLE_NAME))
            self.dvs_pbh.remove_pbh_table(PBH_TABLE_NAME)
            self.dvs_acl.verify_acl_table_count(1)

            # PBH rule
            pbhlogger.info("Remove PBH rule: {}".format(PBH_RULE_NAME))
            self.dvs_pbh.remove_pbh_rule(PBH_TABLE_NAME, PBH_RULE_NAME)
            self.dvs_acl.verify_acl_rule_count(0)
            self.dvs_acl.verify_acl_table_count(0)

            # PBH hash field
            pbhlogger.info("Remove PBH hash field: {}".format(PBH_HASH_FIELD_NAME))
            self.dvs_pbh.remove_pbh_hash_field(PBH_HASH_FIELD_NAME)
            self.dvs_pbh.verify_pbh_hash_field_count(1)

            # PBH hash
            pbhlogger.info("Remove PBH hash: {}".format(PBH_HASH_NAME))
            self.dvs_pbh.remove_pbh_hash(PBH_HASH_NAME)
            self.dvs_pbh.verify_pbh_hash_count(0)
            self.dvs_pbh.verify_pbh_hash_field_count(0)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
