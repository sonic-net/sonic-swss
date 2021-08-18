import redis
import time
import os
import pytest
import re
import json
from swsscommon import swsscommon

PFCWD_TABLE_NAME = "DROP_TEST_TABLE"
PFCWD_TABLE_TYPE = "DROP"
PFCWD_TC = ["3", "4"]
PFCWD_RULE_NAME_1 =  "DROP_TEST_RULE_1"
PFCWD_RULE_NAME_2 =  "DROP_TEST_RULE_2"


# Define fake platform for "DVS" fixture, it will set "platform" environment variable for the "orchagent" code.
# It is needed for the "test_PfcWdAsym" test case because some PFCWD "orchagent" code is under "platform" condition.
# There is no implementation for the virtual switch so as a result PFCWD cannot correctly initialize.
DVS_FAKE_PLATFORM = "mellanox"


def setPortPfc(dvs, port_name, pfc_queues):
    cfg_db = dvs.get_config_db()

    if pfc_queues:
        cfg_db.create_entry('PORT_QOS_MAP', port_name, {'pfc_enable': ','.join(str(q) for q in pfc_queues)})
    else:
        cfg_db.delete_entry('PORT_QOS_MAP', port_name)


def getPortOid(dvs, port_name):
    cnt_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    port_map_tbl = swsscommon.Table(cnt_db, 'COUNTERS_PORT_NAME_MAP')

    for k in port_map_tbl.get('')[1]:
        if k[0] == port_name:
            return k[1]

    return ''


def setPortPfcAsym(dvs, port_name, pfc_asym):
    cfg_db = dvs.get_config_db()

    cfg_db.update_entry('PORT', port_name, {'pfc_asym': pfc_asym})


def startPfcWd(dvs, port_name):
    dvs.runcmd("pfcwd start --action drop --restoration-time 400 {} 400".format(port_name))


def verifyPfcWdCountersList(dvs, port_oid):

    pfc_wd_db = swsscommon.DBConnector(swsscommon.PFC_WD_DB, dvs.redis_sock, 0)
    flex_cnt_tbl = swsscommon.Table(pfc_wd_db, 'FLEX_COUNTER_TABLE')

    expected_counters_list = ['SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION_US',
                              'SAI_PORT_STAT_PFC_0_RX_PKTS',
                              'SAI_PORT_STAT_PFC_1_RX_PKTS',
                              'SAI_PORT_STAT_PFC_2_RX_PKTS',
                              'SAI_PORT_STAT_PFC_3_RX_PKTS',
                              'SAI_PORT_STAT_PFC_4_RX_PKTS',
                              'SAI_PORT_STAT_PFC_5_RX_PKTS',
                              'SAI_PORT_STAT_PFC_6_RX_PKTS',
                              'SAI_PORT_STAT_PFC_7_RX_PKTS']

    counters_list = flex_cnt_tbl.get('PFC_WD:{}'.format(port_oid))[1][0][1].split(',')

    assert sorted(counters_list) == sorted(expected_counters_list), "PFCWD is not started for all PFC priorities"


class TestPfcWd:
    def test_PfcWdAclCreationDeletion(self, dvs, dvs_acl, testlog):
        try:
            dvs_acl.create_acl_table(PFCWD_TABLE_NAME, PFCWD_TABLE_TYPE, ["Ethernet0","Ethernet8", "Ethernet16", "Ethernet24"], stage="ingress")

            config_qualifiers = {
                "TC" : PFCWD_TC[0],
                "IN_PORTS": "Ethernet0"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("3&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet0"])
            }
        
            dvs_acl.create_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_1, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")

            config_qualifiers = {
                "TC" : PFCWD_TC[0],
                "IN_PORTS": "Ethernet0,Ethernet16"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("3&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet0","Ethernet16"])
            }

            dvs_acl.update_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_1, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")
            dvs_acl.remove_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_1)

            config_qualifiers = {
                "TC" : PFCWD_TC[1],
                "IN_PORTS": "Ethernet8"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("4&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet8"]),
            }

            dvs_acl.create_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_2, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")

            config_qualifiers = {
                "TC" : PFCWD_TC[1],
                "IN_PORTS": "Ethernet8,Ethernet24"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("4&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet8","Ethernet24"]),
            }

            dvs_acl.update_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_2, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")
            dvs_acl.remove_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_2)

        finally:
            dvs_acl.remove_acl_table(PFCWD_TABLE_NAME)

    '''
    Verifies that PFC WD starts for all priorities in case of Asymmetric PFC is enabled
    '''
    def test_PfcWdAsym(self, dvs, testlog):

        port_name = 'Ethernet0'
        pfc_queues = [ 3, 4 ]

        # Configure default PFC
        setPortPfc(dvs, port_name, pfc_queues)

        # Get SAI object ID for the interface
        port_oid = getPortOid(dvs, port_name)

        # Enable asymmetric PFC
        setPortPfcAsym(dvs, port_name, 'on')

        # Start PFCWD
        startPfcWd(dvs, port_name)

        # Verify that PFC WD was started for all PFC priorities
        verifyPfcWdCountersList(dvs, port_oid)

        # Disable asymmetric PFC
        setPortPfcAsym(dvs, port_name, 'off')

        # Remove default PFC
        setPortPfc(dvs, port_name, [])


#
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
