from swsscommon import swsscommon

import time
import pytest
import json
import util
import l3_admit


@pytest.mark.usefixtures("dvs_lag_manager")
class TestP4RTL3Admit(object):
    def _set_up(self, dvs):
        self._p4rt_l3_admit_obj = l3_admit.P4RtL3AdmitWrapper()

        self._p4rt_l3_admit_obj.set_up_databases(dvs)

    @pytest.mark.skip(reason="sairedis vs MY MAC support is not ready")
    def test_DefaultL3AdmitAddDeletePass(self, dvs, testlog):
        # Initialize database connectors.
        self._set_up(dvs)

        # Maintain list of original Application and ASIC DB entries before
        # adding new entries
        db_list = (
            (
                self._p4rt_l3_admit_obj.appl_db,
                "%s:%s"
                % (self._p4rt_l3_admit_obj.APP_DB_TBL_NAME, self._p4rt_l3_admit_obj.TBL_NAME),
            ),
            (self._p4rt_l3_admit_obj.asic_db,
             self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_l3_admit_obj.get_original_redis_entries(db_list)

        # l3 admit entry attributes
        # P4RT_TABLE:FIXED_L3_ADMIT_TABLE:{\"match/dst_mac\":\"00:02:03:04:00:00&ff:ff:ff:ff:00:00\",\"match/in_port\":\"Ethernet8\",\"priority\":2030}
        # "action": "admit_to_l3"
        # "controller_metadata": "..."
        dst_mac_data = "00:02:03:04:00:00"
        dst_mac_mask = "FF:FF:FF:FF:00:00"
        in_port = "Ethernet8"
        priority = 2030

        # Create l3 admit entry.
        (
            l3_admit_key,
            attr_list,
        ) = self._p4rt_l3_admit_obj.create_l3_admit(dst_mac_data + "&" + dst_mac_mask, priority, in_port)
        self._p4rt_l3_admit_obj.verify_response(
            l3_admit_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Query application database for l3 admit entries.
        l3_admit_entries = util.get_keys(
            self._p4rt_l3_admit_obj.appl_db,
            self._p4rt_l3_admit_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_l3_admit_obj.TBL_NAME,
        )
        assert len(l3_admit_entries) == (
            self._p4rt_l3_admit_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created l3 admit key.
        (status, fvs) = util.get_key(
            self._p4rt_l3_admit_obj.appl_db,
            self._p4rt_l3_admit_obj.APP_DB_TBL_NAME,
            l3_admit_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for my mac entries.
        asic_l3_admit_entries = util.get_keys(
            self._p4rt_l3_admit_obj.asic_db, self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME
        )
        assert len(asic_l3_admit_entries) == (
            self._p4rt_l3_admit_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for newly created my mac key.
        asic_db_key = self._p4rt_l3_admit_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_l3_admit_obj.asic_db,
            self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [(self._p4rt_l3_admit_obj.SAI_ATTR_DST_MAC, dst_mac_data),
                     (self._p4rt_l3_admit_obj.SAI_ATTR_DST_MAC_MASK, dst_mac_mask),
                     (self._p4rt_l3_admit_obj.SAI_ATTR_PRIORITY, str(priority)),
                     (self._p4rt_l3_admit_obj.SAI_ATTR_PORT_ID, util.get_port_oid_by_name(dvs, in_port))]
        util.verify_attr(fvs, attr_list)

        # deplicate SET will be no-op.
        new_l3_admit_key, new_attr_list = self._p4rt_l3_admit_obj.create_l3_admit(
            dst_mac_data + "&" + dst_mac_mask, priority, in_port)
        self._p4rt_l3_admit_obj.verify_response(
            new_l3_admit_key, new_attr_list,
            "SWSS_RC_SUCCESS",
            "L3 Admit entry with the same key received: 'match/dst_mac=00:02:03:04:00:00&ff:ff:ff:ff:00:00:match/in_port=Ethernet8:priority=2030'"
        )

        # Remove l3 admit entry.
        self._p4rt_l3_admit_obj.remove_app_db_entry(l3_admit_key)
        self._p4rt_l3_admit_obj.verify_response(
            l3_admit_key, [], "SWSS_RC_SUCCESS"
        )

        # Query application database for route entries.
        l3_admit_entries = util.get_keys(
            self._p4rt_l3_admit_obj.appl_db,
            self._p4rt_l3_admit_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_l3_admit_obj.TBL_NAME,
        )
        assert len(l3_admit_entries) == (
            self._p4rt_l3_admit_obj.get_original_appl_db_entries_count()
        )

        # Verify that the route_key no longer exists in application database.
        (status, fsv) = util.get_key(
            self._p4rt_l3_admit_obj.appl_db,
            self._p4rt_l3_admit_obj.APP_DB_TBL_NAME,
            l3_admit_key,
        )
        assert status == False

        # Query ASIC database for my mac entries.
        my_mac_entries = util.get_keys(
            self._p4rt_l3_admit_obj.asic_db, self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME
        )
        assert len(my_mac_entries) == (
            self._p4rt_l3_admit_obj.get_original_asic_db_entries_count()
        )

        # Verify that removed route no longer exists in ASIC database.
        (status, fvs) = util.get_key(
            self._p4rt_l3_admit_obj.asic_db,
            self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == False

    def test_InvalidL3AdmitKeyFailsToCreate(self, dvs, testlog):
        # Initialize database connectors.
        self._set_up(dvs)

        # Maintain list of original Application and ASIC DB entries before
        # adding new entries
        db_list = (
            (
                self._p4rt_l3_admit_obj.appl_db,
                "%s:%s"
                % (self._p4rt_l3_admit_obj.APP_DB_TBL_NAME, self._p4rt_l3_admit_obj.TBL_NAME),
            ),
            (self._p4rt_l3_admit_obj.asic_db,
             self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_l3_admit_obj.get_original_redis_entries(db_list)

        # Invalid l3 admit key
        # P4RT_TABLE:FIXED_L3_ADMIT_TABLE:{\"match/dst_mac\":\"1\",\"match/in_port\":\"Ethernet8\",\"priority\":2030}
        # "action": "admit_to_l3"
        # "controller_metadata": "..."
        dst_mac_data = "1"
        in_port = "Ethernet8"
        priority = 2030

        # Create l3 admit entry.
        (
            l3_admit_key,
            attr_list,
        ) = self._p4rt_l3_admit_obj.create_l3_admit(dst_mac_data, priority, in_port)
        self._p4rt_l3_admit_obj.verify_response(
            l3_admit_key, attr_list,
            "SWSS_RC_INVALID_PARAM",
            "[OrchAgent] Failed to deserialize l3 admit key"
        )

        # Query ASIC database for my mac entries. Count remains the same
        asic_l3_admit_entries = util.get_keys(
            self._p4rt_l3_admit_obj.asic_db, self._p4rt_l3_admit_obj.ASIC_DB_TBL_NAME
        )
        assert len(asic_l3_admit_entries) == (
            self._p4rt_l3_admit_obj.get_original_asic_db_entries_count()
        )

	# Configure lag ports on the switch
        lag_id = "LAG1"
        self.dvs_lag.create_port_channel(lag_id)
        time.sleep(1)

        # Verify that lag port is created.
        self.dvs_lag.get_and_verify_port_channel(1)

        # Invalid l3 admit key - in_port type is not supported
        # P4RT_TABLE:FIXED_L3_ADMIT_TABLE:{\"match/dst_mac\":\"00:02:03:04:00:00&FF:FF:FF:FF:00:00\",\"match/in_port\":\"PortChannelLAG1\",\"priority\":2030}
        # "action": "admit_to_l3"
        # "controller_metadata": "..."
        dst_mac_data = "00:02:03:04:00:00"
        dst_mac_mask = "FF:FF:FF:FF:00:00"
        in_port = "PortChannel" + lag_id
        priority = 2030

        # Create l3 admit entry.
        (l3_admit_key, attr_list,) = self._p4rt_l3_admit_obj.create_l3_admit(
            dst_mac_data + "&" + dst_mac_mask, priority, in_port
        )
        self._p4rt_l3_admit_obj.verify_response(
            l3_admit_key,
            attr_list,
            "SWSS_RC_UNIMPLEMENTED",
            "[OrchAgent] Port \'PortChannelLAG1\'\'s type 5 is not physical and is not supported for L3 Admit entry.",
        )
