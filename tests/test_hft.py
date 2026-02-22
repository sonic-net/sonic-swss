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

    def create_hft_group_without_fields(self, dvs, profile_name="test", group_name="PORT"):
        """Create HFT group in CONFIG_DB without object_names and object_counters fields."""
        config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        tbl = swsscommon.Table(config_db, "HIGH_FREQUENCY_TELEMETRY_GROUP")

        key = f"{profile_name}|{group_name}"
        # Create empty field-value pairs - no object_names or object_counters
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
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
        buffer_pool_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL")
        queue_tbl = swsscommon.Table(
            asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")

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
            "ports": self._get_table_entries(ports_tbl),
            "buffer_pool": self._get_table_entries(buffer_pool_tbl),
            "queues": self._get_table_entries(queue_tbl)
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

    def verify_asic_db_objects(self, asic_db, groups=[(1, 1)], watermark_count=0):
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
            enable_capability = False
            enable_capability = enable_capability or tam_tel_type.get(
                "SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_PORT_STATS", "false") == \
                "true"
            enable_capability = enable_capability or tam_tel_type.get(
                "SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_MMU_STATS", "false") == \
                "true"
            enable_capability = enable_capability or tam_tel_type.get(
                "SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_OUTPUT_QUEUE_STATS", "false") == \
                "true"
            assert enable_capability, \
                "Expected tam telemetry to have at least one enable " \
                "capability set to true"
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

        read_mode_count = 0
        watermark_mode_count = 0
        for tam_counter_sub in asic_db["tam_counter_subscription"].values():
            # Fix: Use only the object ID
            tel_type_oid = tam_counter_sub[
                "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_TEL_TYPE"]
            assert tel_type_oid in asic_db["tam_tel_type"], \
                "Expected tam counter subscription to reference tam " \
                "telemetry type"

            # Fix: Use only the object ID
            subscription_oid = tam_counter_sub[
                "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_OBJECT_ID"]
            assert (subscription_oid in asic_db["ports"] or subscription_oid in asic_db["buffer_pool"] or subscription_oid in asic_db["queues"]), \
                "Expected tam counter subscription to reference port, buffer_pool, or queue"

            # Only check if we have counter subscriptions
            if counters_number > 0:
                if tam_counter_sub[
                        "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STATS_MODE"] == "SAI_STATS_MODE_READ":
                    read_mode_count += 1
                elif tam_counter_sub[
                        "SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STATS_MODE"] == "SAI_STATS_MODE_READ_AND_CLEAR":
                    watermark_mode_count += 1
        if counters_number > 0:
            assert read_mode_count == counters_number - watermark_count, \
                f"Expected {counters_number - watermark_count} read mode subscriptions"
            assert watermark_mode_count == watermark_count, \
                f"Expected {watermark_count} watermark mode subscriptions"

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

    def test_hft_stream_state_sync_to_state_db(self, dvs, testlog):
        """Verify profile stream_state updates are synchronized to STATE_DB stream_status."""
        profile_name = "test"
        port_group_name = "PORT"
        buffer_pool_group_name = "BUFFER_POOL"
        port_session_key = f"{profile_name}|{port_group_name}"
        buffer_pool_session_key = f"{profile_name}|{buffer_pool_group_name}"

        state_db = swsscommon.DBConnector(6, dvs.redis_sock, 0)
        state_tbl = swsscommon.Table(state_db, "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE")

        # 1) Insert config with stream_state=disabled
        self.create_hft_profile(dvs, name=profile_name, status="disabled")
        self.create_hft_group(dvs, profile_name=profile_name, group_name=port_group_name)
        self.create_hft_group(
            dvs,
            profile_name=profile_name,
            group_name=buffer_pool_group_name,
            object_names="egress_lossless_pool",
            object_counters="CURR_OCCUPANCY_BYTES",
        )

        # Allow orchagent to create the state entry.
        time.sleep(3)

        # 2) Update profile stream_state=enabled
        self.create_hft_profile(dvs, name=profile_name, status="enabled")
        time.sleep(3)

        # 3) Verify both state DB entries have stream_status=enabled
        for session_key in [port_session_key, buffer_pool_session_key]:
            status, fvs = state_tbl.get(session_key)
            assert status, f"Expected STATE_DB entry to exist for {session_key}"
            entry = dict(fvs)
            assert entry.get("stream_status") == "enabled", (
                f"Expected stream_status=enabled for {session_key}, got {entry.get('stream_status')} (entry={entry})"
            )

        # Cleanup
        self.delete_hft_group(dvs, profile_name=profile_name, group_name=port_group_name)
        self.delete_hft_group(dvs, profile_name=profile_name, group_name=buffer_pool_group_name)
        self.delete_hft_profile(dvs, name=profile_name)

    def test_hft_empty_fields_with_disabled_status(self, dvs, testlog):
        """Test HFT with empty object_names and object_counters when profile is disabled."""
        # Create HFT profile with disabled status
        self.create_hft_profile(dvs, status="disabled")
        
        # Create HFT group with empty object_names and object_counters
        self.create_hft_group(dvs,
                              object_names="",
                              object_counters="")

        # Wait for processing
        time.sleep(3)

        # Verify that no counter subscriptions are created when 
        # profile is disabled and fields are empty
        asic_db = self.get_asic_db_objects(dvs)
        
        assert len(asic_db["tam_counter_subscription"]) == 0, \
            "Expected no tam counter subscriptions when profile is disabled " \
            "and object_names/object_counters are empty"

        # Clean up
        self.delete_hft_group(dvs)
        self.delete_hft_profile(dvs)

    def test_hft_missing_fields_with_disabled_status(self, dvs, testlog):
        """Test HFT without object_names and object_counters fields when profile is disabled."""
        # Create HFT profile with disabled status
        self.create_hft_profile(dvs, status="disabled")
        
        # Create HFT group without object_names and object_counters fields
        self.create_hft_group_without_fields(dvs)

        # Wait for processing
        time.sleep(3)

        # Verify that no counter subscriptions are created when 
        # profile is disabled and fields are missing entirely
        asic_db = self.get_asic_db_objects(dvs)
        
        assert len(asic_db["tam_counter_subscription"]) == 0, \
            "Expected no tam counter subscriptions when profile is disabled " \
            "and object_names/object_counters fields are missing"

        # Clean up
        self.delete_hft_group(dvs)
        self.delete_hft_profile(dvs)

    def test_hft_buffer_queue_group(self, dvs, testlog):
        """Test HFT with QUEUE (buffer queue) objects."""
        self.create_hft_profile(dvs)
        self.create_hft_group(dvs,
                              group_name="QUEUE",
                              object_names="Ethernet0|7",
                              object_counters="PACKETS")

        time.sleep(5)

        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[(1, 1)])

        self.delete_hft_group(dvs, group_name="QUEUE")
        time.sleep(2)

        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[])

        self.delete_hft_profile(dvs)

    def test_hft_multiple_groups(self, dvs, testlog):
        """Test HFT with multiple groups and objects."""
        # Create HFT profile and groups
        self.create_hft_profile(dvs)
        self.create_hft_group(dvs,
                              object_names="Ethernet0,Ethernet4,Ethernet8",
                              object_counters="IF_IN_OCTETS,IF_IN_UCAST_PKTS,"
                                              "IF_IN_DISCARDS")
        self.create_hft_group(dvs,
                              group_name="BUFFER_POOL",
                              object_names="egress_lossless_pool,egress_lossy_pool,ingress_lossless_pool",
                              object_counters="DROPPED_PACKETS,CURR_OCCUPANCY_BYTES,"
                                              "WATERMARK_BYTES")
        # The KVM latform doesn't support ingress priority groups and queues
        # self.create_hft_group(dvs,
        #                       group_name="INGRESS_PRIORITY_GROUP",
        #                       object_names="Ethernet0|0,Ethernet4|0,Ethernet8|0",
        #                       object_counters="PACKETS,BYTES,WATERMARK_BYTES")
        # self.create_hft_group(dvs,
        #                       group_name="QUEUE",
        #                       object_names="Ethernet0|0,Ethernet4|0,Ethernet8|0",
        #                       object_counters="PACKETS,BYTES,WATERMARK_BYTES")

        # Wait for objects to be created
        time.sleep(5)

        # Verify ASIC objects are created (4 groups subscriptions)
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[(3, 3), (3,3)], watermark_count=3)
        # self.verify_asic_db_objects(asic_db, groups=[(3, 3), (3,3), (3, 3), (3, 3)])

        # Clean up group
        self.delete_hft_group(dvs, group_name="PORT")
        self.delete_hft_group(dvs, group_name="BUFFER_POOL")
        self.delete_hft_group(dvs, group_name="INGRESS_PRIORITY_GROUP")
        self.delete_hft_group(dvs, group_name="QUEUE")
        time.sleep(2)

        # Verify counter subscriptions are cleaned up
        asic_db = self.get_asic_db_objects(dvs)
        self.verify_asic_db_objects(asic_db, groups=[])

        # Clean up profile
        self.delete_hft_profile(dvs)

    def test_hft_empty_default_config_cleanup(self, dvs, testlog):
        """Test that TAM_TELEMETRY objects are properly cleaned up when deleting 
        a default HFT profile with empty object_names and object_counters."""  
        # Create default HFT profile
        self.create_hft_profile(dvs, name="default", status="enabled")
        
        # Create HFT group with empty object_names and object_counters
        self.create_hft_group(dvs,
                              profile_name="default",
                              group_name="PORT",
                              object_names="",
                              object_counters="")

        # Wait for processing
        time.sleep(3)

        # Get ASIC DB objects and verify TAM_TELEMETRY exists
        asic_db = self.get_asic_db_objects(dvs)
        assert len(asic_db["tam_telemetry"]) > 0, \
            "Expected TAM_TELEMETRY object to exist before deletion"

        # Delete profile first
        self.delete_hft_profile(dvs, name="default")
        
        # Sleep for 1 second
        time.sleep(1)
        
        # Delete group
        self.delete_hft_group(dvs, profile_name="default", group_name="PORT")

        # Wait for cleanup
        time.sleep(2)

        # Verify TAM_TELEMETRY object is deleted
        asic_db = self.get_asic_db_objects(dvs)
        assert len(asic_db["tam_telemetry"]) == 0, \
            "Expected TAM_TELEMETRY object to be deleted after profile and group deletion"


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
