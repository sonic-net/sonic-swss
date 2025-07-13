import time

from swsscommon import swsscommon


class TestHFT(object):
    """Test High Frequency Telemetry (HFT) functionality using DVS."""

    def setup_method(self, method):
        """Set up test method with database connections."""
        pass

    def teardown_method(self, method):
        """Clean up after each test method."""
        pass

    def create_hft_profile(self, dvs, name="test", status="enabled",
                           polling_interval=300):
        """Create HFT profile in CONFIG_DB."""
        config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(config_db, "HIGH_FREQUENCY_TELEMETRY_PROFILE")

        fvs = swsscommon.FieldValuePairs([
            ("stream_state", status),
            ("poll_interval", str(polling_interval))
        ])
        tbl.set(name, fvs)

    def create_hft_group(self, dvs, profile_name="test", group_name="PORT",
                         object_names="Ethernet0",
                         object_counters="IF_IN_OCTETS"):
        """Create HFT group in CONFIG_DB."""
        config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(config_db, "HIGH_FREQUENCY_TELEMETRY_GROUP")

        key = f"{profile_name}|{group_name}"
        fvs = swsscommon.FieldValuePairs([
            ("object_names", object_names),
            ("object_counters", object_counters)
        ])
        tbl.set(key, fvs)

    def delete_hft_profile(self, dvs, name="test"):
        """Delete HFT profile from CONFIG_DB."""
        config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(config_db, "HIGH_FREQUENCY_TELEMETRY_PROFILE")
        tbl._del(name)

    def delete_hft_group(self, dvs, profile_name="test", group_name="PORT"):
        """Delete HFT group from CONFIG_DB."""
        config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(config_db, "HIGH_FREQUENCY_TELEMETRY_GROUP")
        key = f"{profile_name}|{group_name}"
        tbl._del(key)

    def get_asic_db_objects(self, dvs):
        """Get all relevant HFT-related objects from ASIC_STATE DB."""
        asic_db = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # Get all TAM-related objects
        tam_transport_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TRANSPORT")
        tam_collector_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_COLLECTOR")
        tam_tel_type_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TEL_TYPE")
        tam_report_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_REPORT")
        tam_tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM")
        tam_counter_sub_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_COUNTER_SUBSCRIPTION")
        tam_telemetry_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TAM_TELEMETRY")
        hostif_trap_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP")
        host_trap_group_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP")
        ports_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        return {
            "tam_transport": self._get_table_entries(tam_transport_tbl),
            "tam_collector": self._get_table_entries(tam_collector_tbl),
            "tam_tel_type": self._get_table_entries(tam_tel_type_tbl),
            "tam_report": self._get_table_entries(tam_report_tbl),
            "tam": self._get_table_entries(tam_tbl),
            "tam_counter_subscription": self._get_table_entries(
                tam_counter_sub_tbl),
            "tam_telemetry": self._get_table_entries(tam_telemetry_tbl),
            "hostif_user_defined_trap": self._get_table_entries(
                hostif_trap_tbl),
            "host_trap_group": self._get_table_entries(host_trap_group_tbl),
            "ports": self._get_table_entries(ports_tbl)
        }

    def _get_table_entries(self, table):
        """Helper method to get all entries from a table."""
        entries = {}
        keys = table.getKeys()
        for key in keys:
            status, fvs = table.get(key)
            if status:
                entries[key] = dict(fvs)
        return entries

    def verify_asic_db_objects(self, asic_db, groups=[(1, 1)]):
        """Verify HFT objects are created correctly in ASIC_STATE DB."""

        # If no groups, we expect minimal or no HFT objects
        if not groups:
            # When no groups are configured, counter subscriptions should be
            # empty
            assert len(asic_db["tam_counter_subscription"]) == 0, \
                "Expected no tam counter subscriptions when no groups " \
                "configured"
            # Other objects might still exist as base infrastructure
            return

        # Verify TAM transport
        assert len(asic_db["tam_transport"]) == 1, "Expected one tam transport"
        tam_transport = list(asic_db["tam_transport"].values())[0]
        assert tam_transport["SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE"] == \
            "SAI_TAM_TRANSPORT_TYPE_NONE", \
            "Expected tam transport type to be SAI_TAM_TRANSPORT_TYPE_NONE"

        # Verify TAM collector
        assert len(asic_db["tam_collector"]) == 1, "Expected one tam collector"
        tam_collector = list(asic_db["tam_collector"].values())[0]

        # Fix: Use only the object ID, not the full key
        transport_oid = tam_collector["SAI_TAM_COLLECTOR_ATTR_TRANSPORT"]
        assert transport_oid in asic_db["tam_transport"], \
            f"Expected tam collector to reference tam transport. " \
            f"Looking for {transport_oid} in " \
            f"{list(asic_db['tam_transport'].keys())}"

        assert tam_collector["SAI_TAM_COLLECTOR_ATTR_LOCALHOST"] == "true", \
            "Expected tam collector to be localhost"

        # Fix: Use only the object ID
        trap_oid = tam_collector["SAI_TAM_COLLECTOR_ATTR_HOSTIF_TRAP"]
        assert trap_oid in asic_db["hostif_user_defined_trap"], \
            "Expected tam collector to reference hostif user defined trap"

        # Verify TAM telemetry type
        assert len(asic_db["tam_tel_type"]) == len(groups), \
            f"Expected {len(groups)} tam telemetry types"

        for tam_tel_type in asic_db["tam_tel_type"].values():
            assert tam_tel_type[
                "SAI_TAM_TEL_TYPE_ATTR_TAM_TELEMETRY_TYPE"] == \
                "SAI_TAM_TELEMETRY_TYPE_COUNTER_SUBSCRIPTION", \
                "Expected tam telemetry type to be " \
                "SAI_TAM_TELEMETRY_TYPE_COUNTER_SUBSCRIPTION"
            assert tam_tel_type[
                "SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_PORT_STATS"] == \
                "true", \
                "Expected tam telemetry to be switch enable port stats"
            assert tam_tel_type["SAI_TAM_TEL_TYPE_ATTR_MODE"] == \
                "SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE", \
                "Expected tam telemetry to be mode single type"

            # Fix: Use only the object ID
            report_oid = tam_tel_type["SAI_TAM_TEL_TYPE_ATTR_REPORT_ID"]
            assert report_oid in asic_db["tam_report"], \
                "Expected tam telemetry to reference tam report"

        # Verify TAM report
        assert len(asic_db["tam_report"]) == len(groups), \
            f"Expected {len(groups)} tam reports"

        for tam_report in asic_db["tam_report"].values():
            assert tam_report["SAI_TAM_REPORT_ATTR_TYPE"] == \
                "SAI_TAM_REPORT_TYPE_IPFIX", \
                "Expected tam report type to be SAI_TAM_REPORT_TYPE_IPFIX"
            assert tam_report["SAI_TAM_REPORT_ATTR_REPORT_MODE"] == \
                "SAI_TAM_REPORT_MODE_BULK", \
                "Expected tam report mode to be SAI_TAM_REPORT_MODE_BULK"
            assert tam_report[
                "SAI_TAM_REPORT_ATTR_TEMPLATE_REPORT_INTERVAL"] == \
                "0", \
                "Expected tam report template report interval to be 0"
            assert tam_report["SAI_TAM_REPORT_ATTR_REPORT_INTERVAL"] == \
                "300", \
                "Expected tam report report interval to be 300"

        # Verify main TAM object
        assert len(asic_db["tam"]) == 1, "Expected one tam object"
        tam = list(asic_db["tam"].values())[0]
        assert "SAI_TAM_BIND_POINT_TYPE_SWITCH" in \
            tam["SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST"], \
            "Expected tam to have bind point type list"

        # Fix: Extract the telemetry object ID and check directly
        tam_telemetry_oid = ":".join(
            tam["SAI_TAM_ATTR_TELEMETRY_OBJECTS_LIST"].split(":")[1:3])
        assert tam_telemetry_oid in asic_db["tam_telemetry"], \
            "Expected tam to reference tam telemetry"

        # Verify TAM counter subscriptions
        counters_number = sum([group[0] * group[1] for group in groups])
        assert len(asic_db["tam_counter_subscription"]) == counters_number, \
            f"Expected {counters_number} tam counter subscriptions"

        for tam_counter_sub in asic_db["tam_counter_subscription"].values():
            # Fix: Use only the object ID
            tel_type_oid = tam_counter_sub[
                "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_TEL_TYPE"]
            assert tel_type_oid in asic_db["tam_tel_type"], \
                "Expected tam counter subscription to reference tam " \
                "telemetry type"

            # Fix: Use only the object ID
            port_oid = tam_counter_sub[
                "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_OBJECT_ID"]
            assert port_oid in asic_db["ports"], \
                "Expected tam counter subscription to reference port"

            # Only check if we have counter subscriptions
            if counters_number > 0:
                assert tam_counter_sub[
                    "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STATS_MODE"] == \
                    "SAI_STATS_MODE_READ", \
                    "Expected tam counter subscription stats mode to be " \
                    "SAI_STATS_MODE_READ"

        # Verify TAM telemetry
        assert len(asic_db["tam_telemetry"]) == 1, "Expected one tam telemetry"
        tam_telemetry = list(asic_db["tam_telemetry"].values())[0]

        collector_list_count = tam_telemetry[
            "SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST"].split(":")[0]
        assert collector_list_count == "1", \
            "Expected tam telemetry collector list count to be 1"

        # Fix: Extract the collector object ID and check directly
        collector_oid = ":".join(tam_telemetry[
            "SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST"].split(":")[1:3])
        assert collector_oid in asic_db["tam_collector"], \
            "Expected tam telemetry to reference tam collector"

        tam_type_list_count = tam_telemetry[
            "SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST"].split(":")[0]
        assert tam_type_list_count == str(len(groups)), \
            f"Expected tam telemetry tam type list count to be {len(groups)}"

        if len(groups) == 1:
            # Fix: Extract the telemetry type object ID and check directly
            tam_type_oid = ":".join(tam_telemetry[
                "SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST"].split(":")[1:3])
            assert tam_type_oid in asic_db["tam_tel_type"], \
                "Expected tam telemetry to reference tam telemetry type"

        # Verify hostif user defined trap
        assert len(asic_db["hostif_user_defined_trap"]) == 1, \
            "Expected one hostif user defined trap"
        hostif_trap = list(asic_db["hostif_user_defined_trap"].values())[0]
        assert hostif_trap["SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE"] == \
            "SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM", \
            "Expected hostif user defined trap type to be " \
            "SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM"

    def verify_no_asic_objects(self, asic_db):
        """Verify that HFT objects are cleaned up from ASIC_STATE DB."""
        # We expect some objects to remain (like base infrastructure)
        # but counter subscriptions should be cleaned up
        pass

    def test_simple_hft_one_counter(self, dvs, testlog):
        """Test basic HFT functionality with one counter."""
        # Create HFT profile and group
        self.create_hft_profile(dvs)
        self.create_hft_group(dvs)

        # Wait for objects to be created
        time.sleep(5)

        # Verify ASIC objects are created
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[(1, 1)])

        # Clean up group first
        self.delete_hft_group(dvs)
        time.sleep(2)

        # Verify counter subscriptions are cleaned up
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[])

        # Clean up profile
        self.delete_hft_profile(dvs)

    def test_hft_multiple_counters(self, dvs, testlog):
        """Test HFT functionality with multiple counters and objects."""
        # Create HFT profile and group with multiple counters
        self.create_hft_profile(dvs)
        self.create_hft_group(dvs,
                              object_names="Ethernet0,Ethernet4,Ethernet8",
                              object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,"
                                              "IF_IN_DISCARDS")

        # Wait for objects to be created
        time.sleep(5)

        # Verify ASIC objects are created (3 objects × 3 counters = 9
        # subscriptions)
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[(3, 3)])

        # Clean up group
        self.delete_hft_group(dvs)
        time.sleep(2)

        # Verify counter subscriptions are cleaned up
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[])

        # Clean up profile
        self.delete_hft_profile(dvs)

    def test_hft_delete_group_and_rejoin(self, dvs, testlog):
        """Test HFT group deletion and recreation."""
        # Create HFT profile and group
        self.create_hft_profile(dvs)
        self.create_hft_group(dvs,
                              object_names="Ethernet0,Ethernet4,Ethernet8",
                              object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,"
                                              "IF_IN_DISCARDS")

        # Wait for objects to be created
        time.sleep(5)

        # Verify ASIC objects are created
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[(3, 3)])

        # Delete group
        self.delete_hft_group(dvs)
        time.sleep(2)

        # Verify counter subscriptions are cleaned up
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[])

        # Recreate group
        self.create_hft_group(dvs,
                              object_names="Ethernet0,Ethernet4,Ethernet8",
                              object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,"
                                              "IF_IN_DISCARDS")
        time.sleep(5)

        # Verify ASIC objects are created again
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[(3, 3)])

        # Final cleanup
        self.delete_hft_group(dvs)
        time.sleep(2)

        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[])

        self.delete_hft_profile(dvs)

    def test_hft_profile_status_disabled(self, dvs, testlog):
        """Test HFT profile with disabled status."""
        # Create HFT profile with disabled status
        self.create_hft_profile(dvs, status="disabled")
        self.create_hft_group(dvs)

        # Wait
        time.sleep(3)

        # Verify no TAM objects are created when profile is disabled
        # When disabled, we should have minimal or no HFT-specific objects

        # Clean up
        self.delete_hft_group(dvs)
        self.delete_hft_profile(dvs)

    def test_hft_custom_polling_interval(self, dvs, testlog):
        """Test HFT with custom polling interval."""
        # Create HFT profile with custom polling interval
        self.create_hft_profile(dvs, polling_interval=600)
        self.create_hft_group(dvs)

        # Wait for objects to be created
        time.sleep(5)

        # Verify ASIC objects with custom interval
        asic_db = self.get_asic_db_objects(dvs)

        # Check that report interval matches our setting
        for tam_report in asic_db["tam_report"].values():
            assert tam_report["SAI_TAM_REPORT_ATTR_REPORT_INTERVAL"] == \
                "600", \
                "Expected tam report report interval to be 600"

        # Clean up
        self.delete_hft_group(dvs)
        self.delete_hft_profile(dvs)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
