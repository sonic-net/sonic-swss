import time
import pytest

from swsscommon import swsscommon

# Supported drop counters
PORT_INGRESS_DROPS = 'PORT_INGRESS_DROPS'
PORT_EGRESS_DROPS = 'PORT_EGRESS_DROPS'
SWITCH_INGRESS_DROPS = 'SWITCH_INGRESS_DROPS'
SWITCH_EGRESS_DROPS = 'SWITCH_EGRESS_DROPS'

# Debug Counter Table
DEBUG_COUNTER_TABLE = 'DEBUG_COUNTER'
DROP_REASON_TABLE = 'DEBUG_COUNTER_DROP_REASON'

# Debug Counter Capability Table
CAPABILITIES_TABLE = 'DEBUG_COUNTER_CAPABILITIES'
CAP_COUNT = 'count'
CAP_REASONS = 'reasons'
SUPPORTED_COUNTER_CAPABILITIES = [CAP_COUNT, CAP_REASONS]
INGRESS_COUNTER_TYPES = [PORT_INGRESS_DROPS, SWITCH_INGRESS_DROPS]
EGRESS_COUNTER_TYPES = [PORT_EGRESS_DROPS, SWITCH_EGRESS_DROPS]
SUPPORTED_COUNTER_TYPES = INGRESS_COUNTER_TYPES + EGRESS_COUNTER_TYPES

# Debug Counter Flex Counter Group
FLEX_COUNTER_GROUP_TABLE = 'FLEX_COUNTER_GROUP_TABLE'
DEBUG_COUNTER_FLEX_GROUP = 'DEBUG_COUNTER'
FLEX_STATS_MODE_FIELD = 'STATS_MODE'
FLEX_POLL_INTERVAL_FIELD = 'POLL_INTERVAL'
FLEX_STATUS_FIELD = 'FLEX_COUNTER_STATUS'
FLEX_STATUS_ENABLE = 'enable'
EXPECTED_FLEX_STATS_MODE = 'STATS_MODE_READ'
EXPECTED_POLL_INTERVAL_THRESHOLD = 0
EXPECTED_FLEX_GROUP_FIELDS = [FLEX_STATS_MODE_FIELD, FLEX_POLL_INTERVAL_FIELD, FLEX_STATUS_FIELD]

# Debug Counter Flex Counters
FLEX_COUNTER_TABLE = 'FLEX_COUNTER_TABLE:DEBUG_COUNTER'
PORT_DEBUG_COUNTER_LIST = 'PORT_DEBUG_COUNTER_ID_LIST'
SWITCH_DEBUG_COUNTER_LIST = 'SWITCH_DEBUG_COUNTER_ID_LIST'
PORT_STAT_BASE = 'SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS'
PORT_STAT_INDEX_1 = 'SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS'
SWITCH_STAT_BASE = 'SAI_SWITCH_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS'
SWITCH_STAT_INDEX_1 = 'SAI_SWITCH_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS'

# ASIC DB Fields
ASIC_STATE_TABLE = 'ASIC_STATE:SAI_OBJECT_TYPE_DEBUG_COUNTER'
ASIC_COUNTER_TYPE_FIELD = 'SAI_DEBUG_COUNTER_ATTR_TYPE'
ASIC_COUNTER_INGRESS_REASON_LIST_FIELD = 'SAI_DEBUG_COUNTER_ATTR_IN_DROP_REASON_LIST'
ASIC_COUNTER_EGRESS_REASON_LIST_FIELD = 'SAI_DEBUG_COUNTER_ATTR_OUT_DROP_REASON_LIST'
ASIC_COUNTER_PORT_IN_TYPE = 'SAI_DEBUG_COUNTER_TYPE_PORT_IN_DROP_REASONS'
ASIC_COUNTER_PORT_OUT_TYPE = 'SAI_DEBUG_COUNTER_TYPE_PORT_OUT_DROP_REASONS'
ASIC_COUNTER_SWITCH_IN_TYPE = 'SAI_DEBUG_COUNTER_TYPE_SWITCH_IN_DROP_REASONS'
ASIC_COUNTER_SWITCH_OUT_TYPE = 'SAI_DEBUG_COUNTER_TYPE_SWITCH_OUT_DROP_REASONS'
EXPECTED_ASIC_FIELDS = [ASIC_COUNTER_TYPE_FIELD, ASIC_COUNTER_INGRESS_REASON_LIST_FIELD, ASIC_COUNTER_EGRESS_REASON_LIST_FIELD]
EXPECTED_NUM_ASIC_FIELDS = 2

# port to be add and removed
PORT = "Ethernet0"
PORT_TABLE_NAME = "PORT"

@pytest.mark.usefixtures('dvs_port_manager')
# FIXME: It is really annoying to have to re-run tests due to inconsistent timing, should
# implement some sort of polling interface for checking ASIC/flex counter tables after
# applying changes to config DB
class TestDropCounters(object):
    def setup_db(self, dvs):
        self.asic_db = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.flex_db = swsscommon.DBConnector(5, dvs.redis_sock, 0)
        self.state_db = swsscommon.DBConnector(6, dvs.redis_sock, 0)
        self.counters_db = swsscommon.DBConnector(2, dvs.redis_sock, 0)

    def genericGetAndAssert(self, table, key):
        status, fields = table.get(key)
        assert status
        return fields

    def checkFlexCounterGroup(self):
        flex_group_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_GROUP_TABLE)
        status, group_vars = flex_group_table.get(DEBUG_COUNTER_FLEX_GROUP)
        assert status
        assert len(group_vars) == len(EXPECTED_FLEX_GROUP_FIELDS)

        for var in group_vars:
            assert len(var) == 2
            group_field = var[0]
            group_contents = var[1]

            assert group_field in EXPECTED_FLEX_GROUP_FIELDS

            if group_field == FLEX_STATS_MODE_FIELD:
                assert group_contents == EXPECTED_FLEX_STATS_MODE
            elif group_field == FLEX_POLL_INTERVAL_FIELD:
                assert int(group_contents) > EXPECTED_POLL_INTERVAL_THRESHOLD
            elif group_field == FLEX_STATUS_FIELD:
                assert group_contents == FLEX_STATUS_ENABLE
            else:
                assert False

    def checkFlexState(self, stats, counter_list_type):
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        for oid in flex_counter_table.getKeys():
            attributes = self.genericGetAndAssert(flex_counter_table, oid)
            assert len(attributes) == 1
            field, tracked_stats = attributes[0]
            assert field == counter_list_type
            for stat in stats:
                assert stat in tracked_stats

    def checkASICState(self, counter, counter_type, reasons):
        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        asic_counter_params = self.genericGetAndAssert(asic_state_table, counter)
        if len(asic_counter_params) != EXPECTED_NUM_ASIC_FIELDS:
            return False

        for param in asic_counter_params:
            param_name = param[0]
            param_contents = param[1]

            if param_name not in EXPECTED_ASIC_FIELDS:
                return False

            if param_name == ASIC_COUNTER_TYPE_FIELD:
                if param_contents != counter_type:
                    return False
            elif param_name == ASIC_COUNTER_INGRESS_REASON_LIST_FIELD:
                if int(param_contents[0]) != len(reasons):
                    return False

                for reason in reasons:
                    if reason not in param_contents:
                        return False
            else:
                return False

        return True

    def create_drop_counter(self, name, counter_type):
        debug_counter_table = swsscommon.Table(self.config_db, DEBUG_COUNTER_TABLE)
        counter_metadata = swsscommon.FieldValuePairs([('type', counter_type)])
        debug_counter_table.set(name, counter_metadata)

    def add_drop_reason(self, name, drop_reason):
        drop_reason_table = swsscommon.Table(self.config_db, '{}|{}'.format(DROP_REASON_TABLE, name))
        drop_reason_entry = swsscommon.FieldValuePairs([('NULL', 'NULL')])
        drop_reason_table.set(drop_reason, drop_reason_entry)

    def remove_drop_reason(self, name, drop_reason):
        drop_reason_table = swsscommon.Table(self.config_db, '{}|{}'.format(DROP_REASON_TABLE, name))
        drop_reason_table._del(drop_reason)

    def delete_drop_counter(self, name):
        debug_counter_table = swsscommon.Table(self.config_db, DEBUG_COUNTER_TABLE)
        debug_counter_table._del(name)

    def test_deviceCapabilitiesTablePopulated(self, dvs, testlog):
        """
            This test verifies that DebugCounterOrch has succesfully queried
            the capabilities for this device and populated state DB with the
            correct values.
        """
        self.setup_db(dvs)

        # Check that the capabilities table 1) exists and 2) has been populated
        # for each type of counter
        capabilities_table = swsscommon.Table(self.state_db, CAPABILITIES_TABLE)
        counter_types = capabilities_table.getKeys()
        assert len(counter_types) == len(SUPPORTED_COUNTER_TYPES)

        # Check that the data for each counter type is consistent
        for counter_type in SUPPORTED_COUNTER_TYPES:
            assert counter_type in counter_types

            # By definiton, each capability entry should contain exactly the same fields
            counter_capabilities = self.genericGetAndAssert(capabilities_table, counter_type)
            assert len(counter_capabilities) == len(SUPPORTED_COUNTER_CAPABILITIES)

            # Check that the values of each field actually match the
            # capabilities currently defined in the virtual switch
            for capability in counter_capabilities:
                assert len(capability) == 2
                capability_name = capability[0]
                capability_contents = capability[1]

                assert capability_name in SUPPORTED_COUNTER_CAPABILITIES

                if capability_name == CAP_COUNT:
                    assert int(capability_contents) == 3
                elif capability_name == CAP_REASONS and counter_type in INGRESS_COUNTER_TYPES:
                    assert len(capability_contents.split(',')) == 3
                elif capability_name == CAP_REASONS and counter_type in EGRESS_COUNTER_TYPES:
                    assert len(capability_contents.split(',')) == 2
                else:
                    assert False

    def test_flexCounterGroupInitialized(self, dvs, testlog):
        """
            This test verifies that DebugCounterOrch has succesfully
            setup a flex counter group for the drop counters.
        """
        self.setup_db(dvs)
        self.checkFlexCounterGroup()

    def test_createAndRemoveDropCounterBasic(self, dvs, testlog):
        """
            This test verifies that a drop counter can succesfully be
            created and deleted.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'TEST'
        reason = 'L3_ANY'

        self.create_drop_counter(name, PORT_INGRESS_DROPS)

        # Because no reasons have been added to the counter yet, nothing should
        # be put in ASIC DB and the flex counters should not start polling yet.
        assert len(asic_state_table.getKeys()) == 0
        assert len(flex_counter_table.getKeys()) == 0

        self.add_drop_reason(name, reason)
        time.sleep(3)

        # Verify that the flex counters have been created to poll the new
        # counter.
        self.checkFlexState([PORT_STAT_BASE], PORT_DEBUG_COUNTER_LIST)

        # Verify that the drop counter has been added to ASIC DB with the
        # correct reason added.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_PORT_IN_TYPE, [reason])

        self.delete_drop_counter(name)
        time.sleep(3)

        # Verify that the counter has been removed from ASIC DB and the flex
        # counters have been torn down.
        assert len(asic_state_table.getKeys()) == 0
        assert len(flex_counter_table.getKeys()) == 0

        # Cleanup for the next test.
        self.remove_drop_reason(name, reason)

    def test_createAndRemoveDropCounterReversed(self, dvs, testlog):
        """
            This test verifies that a drop counter can succesfully be created
            and deleted when the drop reasons are added before the counter is
            created.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'TEST'
        reason = 'L3_ANY'

        self.add_drop_reason(name, reason)

        # Because the actual counter has not been created yet, nothing should
        # be put in ASIC DB and the flex counters should not start polling yet.
        assert len(asic_state_table.getKeys()) == 0
        assert len(flex_counter_table.getKeys()) == 0

        self.create_drop_counter(name, PORT_INGRESS_DROPS)
        time.sleep(3)

        # Verify that the flex counters have been created to poll the new
        # counter.
        self.checkFlexState([PORT_STAT_BASE], PORT_DEBUG_COUNTER_LIST)

        # Verify that the drop counter has been added to ASIC DB with the
        # correct reason added.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_PORT_IN_TYPE, [reason])

        self.delete_drop_counter(name)
        time.sleep(3)

        # Verify that the counter has been removed from ASIC DB and the flex
        # counters have been torn down.
        assert len(asic_state_table.getKeys()) == 0
        assert len(flex_counter_table.getKeys()) == 0

        # Cleanup for the next test.
        self.remove_drop_reason(name, reason)

    def test_createCounterWithInvalidCounterType(self, dvs, testlog):
        """
            This test verifies that the state of the system is unaffected
            when an invalid counter type is passed to CONFIG DB.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'BAD_CTR'
        reason = 'L3_ANY'

        self.create_drop_counter(name, 'THIS_IS_DEFINITELY_NOT_A_VALID_COUNTER_TYPE')
        self.add_drop_reason(name, reason)
        time.sleep(3)

        # Verify that nothing has been added to ASIC DB and no flex counters
        # were created.
        assert len(asic_state_table.getKeys()) == 0
        assert len(flex_counter_table.getKeys()) == 0

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason)

    def test_createCounterWithInvalidDropReason(self, dvs, testlog):
        """
            This test verifies that the state of the system is unaffected
            when an invalid drop reason is passed to CONFIG DB.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'BAD_CTR'
        reason = 'THIS_IS_DEFINITELY_NOT_A_VALID_DROP_REASON'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason)
        time.sleep(3)

        # Verify that nothing has been added to ASIC DB and no flex counters
        # were created.
        assert len(asic_state_table.getKeys()) == 0
        assert len(flex_counter_table.getKeys()) == 0

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason)

    def test_addReasonToInitializedCounter(self, dvs, testlog):
        """
            This test verifies that a drop reason can be added to a counter
            that has already been initialized.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        time.sleep(3)

        # Verify that a counter has been created. We will verify the state of
        # the counter in the next step.
        assert len(asic_state_table.getKeys()) == 1
        self.checkFlexState([SWITCH_STAT_BASE], SWITCH_DEBUG_COUNTER_LIST)

        reason2 = 'ACL_ANY'
        self.add_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the drop counter has been added to ASIC DB, including the
        # reason that was added.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1, reason2])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)
        self.remove_drop_reason(name, reason2)

    def test_removeReasonFromInitializedCounter(self, dvs, testlog):
        """
            This test verifies that a drop reason can be removed from a counter
            that has already been initialized without deleting the counter.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'
        reason2 = 'ACL_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        self.add_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that a counter has been created. We will verify the state of
        # the counter in the next step.
        assert len(asic_state_table.getKeys()) == 1
        self.checkFlexState([SWITCH_STAT_BASE], SWITCH_DEBUG_COUNTER_LIST)

        self.remove_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the drop counter has been added to ASIC DB, excluding the
        # reason that was removed.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)

    def test_removeAllDropReasons(self, dvs, testlog):
        """
            This test verifies that it is not possible to remove all drop
            reasons from a drop counter.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        time.sleep(3)

        # Verify that a counter has been created. We will verify the state of
        # the counter in the next step.
        assert len(asic_state_table.getKeys()) == 1
        self.checkFlexState([SWITCH_STAT_BASE], SWITCH_DEBUG_COUNTER_LIST)

        self.remove_drop_reason(name, reason1)
        time.sleep(3)

        # Verify that the drop counter has been added to ASIC DB, including the
        # last reason that we attempted to remove.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        # Cleanup for the next test.
        self.delete_drop_counter(name)

    def test_addDropReasonMultipleTimes(self, dvs, testlog):
        """
            This test verifies that the same drop reason can be added multiple
            times without affecting the system.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        time.sleep(3)

        # Verify that a counter has been created. We will verify the state of
        # the counter in the next step.
        assert len(asic_state_table.getKeys()) == 1
        self.checkFlexState([SWITCH_STAT_BASE], SWITCH_DEBUG_COUNTER_LIST)

        reason2 = 'ACL_ANY'
        self.add_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the drop counter has been added to ASIC DB, including the
        # reason that was added.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1, reason2])

        self.add_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the ASIC state is the same as before adding the redundant
        # drop reason.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1, reason2])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)
        self.remove_drop_reason(name, reason2)

    def test_addInvalidDropReason(self, dvs, testlog):
        """
            This test verifies that adding a drop reason to a counter that is
            not recognized will not affect the system.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        time.sleep(3)

        # Verify that a counter has been created. We will verify the state of
        # the counter in the next step.
        assert len(asic_state_table.getKeys()) == 1
        self.checkFlexState([SWITCH_STAT_BASE], SWITCH_DEBUG_COUNTER_LIST)

        reason2 = 'ACL_ANY'
        self.add_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the drop counter has been added to ASIC DB, including the
        # reason that was added.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1, reason2])

        dummy_reason = 'ZOBOOMBAFOO'
        self.add_drop_reason(name, dummy_reason)
        time.sleep(3)

        # Verify that the ASIC state is the same as before adding the invalid
        # drop reason.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1, reason2])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)
        self.remove_drop_reason(name, reason2)

    def test_removeDropReasonMultipleTimes(self, dvs, testlog):
        """
            This test verifies that removing a drop reason multiple times will
            not affect the system.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'
        reason2 = 'ACL_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        self.add_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that a counter has been created. We will verify the state of
        # the counter in the next step.
        assert len(asic_state_table.getKeys()) == 1
        self.checkFlexState([SWITCH_STAT_BASE], SWITCH_DEBUG_COUNTER_LIST)

        self.remove_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the drop counter has been added to ASIC DB, excluding the
        # reason that was removed.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        self.remove_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the ASIC state is the same as before the redundant
        # remove operation.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)

    def test_removeNonexistentDropReason(self, dvs, testlog):
        """
            This test verifies that removing a drop reason that does not exist
            on the device will not affect the system.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'
        reason2 = 'ACL_ANY'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        time.sleep(3)

        # Verify the counter has been created and is in the correct state.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        self.remove_drop_reason(name, reason2)
        time.sleep(3)

        # Verify that the ASIC state is unchanged after the nonexistent remove.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)

    def test_removeInvalidDropReason(self, dvs, testlog):
        """
            This test verifies that removing a drop reason that is not recognized
            will not affect the system.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name = 'ADD_TEST'
        reason1 = 'L3_ANY'
        bogus_reason = 'LIVE_LAUGH_LOVE'

        self.create_drop_counter(name, SWITCH_INGRESS_DROPS)
        self.add_drop_reason(name, reason1)
        time.sleep(3)

        # Verify the counter has been created and is in the correct state.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        self.remove_drop_reason(name, bogus_reason)
        time.sleep(3)

        # Verify that the ASIC state is unchanged after the bad remove.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_SWITCH_IN_TYPE, [reason1])

        # Cleanup for the next test.
        self.delete_drop_counter(name)
        self.remove_drop_reason(name, reason1)
    
    def getPortOid(self, dvs, port_name):
        port_name_map = swsscommon.Table(self.counters_db, "COUNTERS_PORT_NAME_MAP")
        status, returned_value = port_name_map.hget("", port_name);
        assert status == True
        return returned_value
    
    def test_add_remove_port(self, dvs, testlog):
        """
            This test verifies that debug counters are removed when we remove a port 
            and debug counters are added each time we add ports (if debug counter is enabled)
        """
        self.setup_db(dvs)
         
        # save port info
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(cdb, PORT_TABLE_NAME)
        (status, fvs) = tbl.get(PORT)      
        assert status == True
 
        # get counter oid
        oid = self.getPortOid(dvs, PORT)

        # verifies debug coutner dont exist for port
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)
        status, fields = flex_counter_table.get(oid)
        assert len(fields) == 0
 
        # add debug counters
        name1 = 'DEBUG_0'
        reason1 = 'L3_ANY'
        name2 = 'DEBUG_1'
        reason2 = 'L2_ANY'
        
        self.create_drop_counter(name1, PORT_INGRESS_DROPS)
        self.add_drop_reason(name1, reason1)
        
        self.create_drop_counter(name2, PORT_EGRESS_DROPS)
        self.add_drop_reason(name2, reason2)
        time.sleep(3)
 
        # verifies debug counter exist for port
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)
        status, fields = flex_counter_table.get(oid)
        assert status == True
        assert len(fields) == 1
         
        # remove port and wait until it was removed from ASIC DB
        self.dvs_port.remove_port(PORT)
        dvs.get_asic_db().wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", oid)

        # verify that debug counter were removed
        status, fields = flex_counter_table.get(oid)
        assert len(fields) == 0
 
        # add port and wait until the port is added on asic db
        num_of_keys_without_port = len(dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
        tbl.set(PORT, fvs)
        dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_of_keys_without_port + 1)
        dvs.get_counters_db().wait_for_fields("COUNTERS_PORT_NAME_MAP", "", [PORT])
        
        # verifies that debug counters were added for port
        oid = self.getPortOid(dvs, PORT)
        status, fields = flex_counter_table.get(oid)
        assert status == True
        assert len(fields) == 1
        
        # Cleanup for the next test.
        self.delete_drop_counter(name1)
        self.remove_drop_reason(name1, reason1)
        
        self.delete_drop_counter(name2)
        self.remove_drop_reason(name2, reason2)

    def test_createAndDeleteMultipleCounters(self, dvs, testlog):
        """
            This test verifies that creating and deleting multiple drop counters
            at the same time works correctly.
        """
        self.setup_db(dvs)

        asic_state_table = swsscommon.Table(self.asic_db, ASIC_STATE_TABLE)
        flex_counter_table = swsscommon.Table(self.flex_db, FLEX_COUNTER_TABLE)

        name1 = 'DEBUG_0'
        reason1 = 'L3_ANY'

        name2 = 'DEBUG_1'
        reason2 = 'ACL_ANY'

        self.create_drop_counter(name1, PORT_INGRESS_DROPS)
        self.add_drop_reason(name1, reason1)

        self.create_drop_counter(name2, PORT_INGRESS_DROPS)
        self.add_drop_reason(name2, reason2)

        time.sleep(5)

        # Verify that the flex counters are correctly tracking two different
        # drop counters.
        self.checkFlexState([PORT_STAT_BASE, PORT_STAT_INDEX_1], PORT_DEBUG_COUNTER_LIST)

        # Verify that there are two entries in the ASIC DB, one for each counter.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 2
        for key in asic_keys:
            assert (self.checkASICState(key, ASIC_COUNTER_PORT_IN_TYPE, [reason1]) or self.checkASICState(key, ASIC_COUNTER_PORT_IN_TYPE, [reason2]))

        self.delete_drop_counter(name2)
        self.remove_drop_reason(name2, reason2)
        time.sleep(3)

        # Verify that the flex counters are tracking ONE drop counter after
        # the update.
        self.checkFlexState([PORT_STAT_BASE], PORT_DEBUG_COUNTER_LIST)

        # Verify that there is ONE entry in the ASIC DB after the update.
        asic_keys = asic_state_table.getKeys()
        assert len(asic_keys) == 1
        assert self.checkASICState(asic_keys[0], ASIC_COUNTER_PORT_IN_TYPE, [reason1])

        # Cleanup for the next test.
        self.delete_drop_counter(name1)
        self.remove_drop_reason(name1, reason1)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
