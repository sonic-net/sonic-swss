# Lint as: python3
from swsscommon import swsscommon

import pytest
import util
import json
import test_vrf


class P4RtTunnelDecapWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT tunnel decap group object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = swsscommon.APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME
    ACTION = "action"
    VRF_ID = "vrf_id"

    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE = "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE"
    SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE = "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE"
    SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP = "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP"
    SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK = "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK"
    SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID = "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID"
    SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID = "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID"

    def generate_app_db_key(self, ipv6):
        d = {}
        d[util.prepend_match_field("dst_ipv6")] = ipv6
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key


class TestP4RTunnelDecap(object):
    def _set_up(self, dvs):
        self._p4rt_tunnel_decap_wrapper = P4RtTunnelDecapWrapper()
        self._vrf_obj = test_vrf.TestVrf()

        self._p4rt_tunnel_decap_wrapper.set_up_databases(dvs)

    def _set_vrf(self, dvs):
        # Create VRF.
        self._vrf_obj.setup_db(dvs)
        self.vrf_id = "b4-traffic"
        self.vrf_state = self._vrf_obj.vrf_create(dvs, self.vrf_id, [], {})

    def _clean_vrf(self, dvs):
        # Remove VRF.
        self._vrf_obj.vrf_remove(dvs, self.vrf_id, self.vrf_state)

    def test_TunnelDecapGroupAddModifyAndDelete(self, dvs, testlog):
        # Initialize database connectors
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Maintain list of original Application and ASIC DB entries before adding
        # new tunnel decap group
        original_appl_tunnel_decap_group_entries = util.get_keys(
            self._p4rt_tunnel_decap_wrapper.appl_db,
            self._p4rt_tunnel_decap_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_tunnel_decap_wrapper.TBL_NAME)
        original_asic_tunnel_decap_group_entries = util.get_keys(
            self._p4rt_tunnel_decap_wrapper.asic_db, self._p4rt_tunnel_decap_wrapper.ASIC_DB_TBL_NAME)

        # 1. Create tunnel decap group
        ipv6 = "2001:db8:3c4d:15::&ffff:ffff:ffff:ffff::"
        action = "mark_for_tunnel_decap_and_set_vrf"
        vrf_id = "b4-traffic"

        attr_list_in_app_db = [(self._p4rt_tunnel_decap_wrapper.ACTION, action),
                               (util.prepend_param_field(
                                   self._p4rt_tunnel_decap_wrapper.VRF_ID), vrf_id)]
        tunnel_decap_group_key = self._p4rt_tunnel_decap_wrapper.generate_app_db_key(
            ipv6)
        self._p4rt_tunnel_decap_wrapper.set_app_db_entry(
            tunnel_decap_group_key, attr_list_in_app_db)
        self._p4rt_tunnel_decap_wrapper.verify_response(
            tunnel_decap_group_key, attr_list_in_app_db, "SWSS_RC_SUCCESS")

        # Query application database for tunnel decap group entries
        appl_tunnel_decap_group_entries = util.get_keys(
            self._p4rt_tunnel_decap_wrapper.appl_db,
            self._p4rt_tunnel_decap_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_tunnel_decap_wrapper.TBL_NAME)
        assert len(appl_tunnel_decap_group_entries) == len(
            original_appl_tunnel_decap_group_entries) + 1

        # Query application database for newly created tunnel decap group key
        (status, fvs) = util.get_key(self._p4rt_tunnel_decap_wrapper.appl_db,
                                     self._p4rt_tunnel_decap_wrapper.APP_DB_TBL_NAME,
                                     tunnel_decap_group_key)
        assert status == True
        util.verify_attr(fvs, attr_list_in_app_db)

        # Query ASIC database for tunnel decap group entries
        asic_tunnel_decap_group_entries = util.get_keys(self._p4rt_tunnel_decap_wrapper.asic_db,
                                                        self._p4rt_tunnel_decap_wrapper.ASIC_DB_TBL_NAME)
        assert len(asic_tunnel_decap_group_entries) == len(
            original_asic_tunnel_decap_group_entries) + 1

        # Query ASIC database for newly created tunnel decap group key
        asic_db_key = None
        for key in asic_tunnel_decap_group_entries:
            # Get newly created entry
            if key not in original_asic_tunnel_decap_group_entries:
                asic_db_key = key
                break
        assert asic_db_key is not None
        (status, fvs) = util.get_key(self._p4rt_tunnel_decap_wrapper.asic_db,
                                     self._p4rt_tunnel_decap_wrapper.ASIC_DB_TBL_NAME,
                                     asic_db_key)
        assert status == True

        # Get oid of dummy tunnel
        dummy_tunnel_oid = fvs[5][1]
        assert dummy_tunnel_oid != None

        expected_attr_list_in_asic_db = [
            (self._p4rt_tunnel_decap_wrapper.SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE,
             "SAI_TUNNEL_TYPE_IPINIP"),
            (self._p4rt_tunnel_decap_wrapper.SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE,
             "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP"),
            (self._p4rt_tunnel_decap_wrapper.SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP,
             "2001:db8:3c4d:15::"),
            (self._p4rt_tunnel_decap_wrapper.SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK,
             "ffff:ffff:ffff:ffff::"),
            (self._p4rt_tunnel_decap_wrapper.SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID,
             self.vrf_state['entry_id']),
            (self._p4rt_tunnel_decap_wrapper.SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID, dummy_tunnel_oid)
        ]
        util.verify_attr(fvs, expected_attr_list_in_asic_db)

        # 2. Delete the tunnel decap group.
        self._p4rt_tunnel_decap_wrapper.remove_app_db_entry(
            tunnel_decap_group_key)
        self._p4rt_tunnel_decap_wrapper.verify_response(
            tunnel_decap_group_key, [], "SWSS_RC_SUCCESS")

        # Query application database for tunnel decap group entries
        appl_tunnel_decap_group_entries = util.get_keys(
            self._p4rt_tunnel_decap_wrapper.appl_db,
            self._p4rt_tunnel_decap_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_tunnel_decap_wrapper.TBL_NAME)
        assert len(appl_tunnel_decap_group_entries) == len(
            original_appl_tunnel_decap_group_entries)

        # Query application database for the deleted tunnel decap group key
        (status, fvs) = util.get_key(self._p4rt_tunnel_decap_wrapper.appl_db,
                                     self._p4rt_tunnel_decap_wrapper.APP_DB_TBL_NAME,
                                     tunnel_decap_group_key)
        assert status == False

        # Query ASIC database for tunnel decap group entries
        asic_tunnel_decap_group_entries = util.get_keys(self._p4rt_tunnel_decap_wrapper.asic_db,
                                                        self._p4rt_tunnel_decap_wrapper.ASIC_DB_TBL_NAME)
        assert len(asic_tunnel_decap_group_entries) == len(
            original_appl_tunnel_decap_group_entries)

        # Query ASIC state database for the deleted tunnel decap group key
        (status, fvs) = util.get_key(self._p4rt_tunnel_decap_wrapper.asic_db,
                                     self._p4rt_tunnel_decap_wrapper.ASIC_DB_TBL_NAME,
                                     asic_db_key)
        assert status == False

    def test_TunnelDecapGroupModifyNotImplemented(self, dvs, testlog):
        # Initialize database connectors
        self._set_up(dvs)

        # Create tunnel decap group
        ipv6 = "2001:db8:3c4d:15::&ffff:ffff:ffff:ffff::"
        action = "mark_for_tunnel_decap_and_set_vrf"
        vrf_id = "b4-traffic"

        attr_list_in_app_db = [(self._p4rt_tunnel_decap_wrapper.ACTION, action),
                               (util.prepend_param_field(
                                   self._p4rt_tunnel_decap_wrapper.VRF_ID), vrf_id)]
        tunnel_decap_group_key = self._p4rt_tunnel_decap_wrapper.generate_app_db_key(
            ipv6)
        self._p4rt_tunnel_decap_wrapper.set_app_db_entry(
            tunnel_decap_group_key, attr_list_in_app_db)
        self._p4rt_tunnel_decap_wrapper.verify_response(
            tunnel_decap_group_key, attr_list_in_app_db, "SWSS_RC_SUCCESS")

        # Update tunnel decap group fails
        self._p4rt_tunnel_decap_wrapper.set_app_db_entry(
            tunnel_decap_group_key, attr_list_in_app_db)
        self._p4rt_tunnel_decap_wrapper.verify_response(
            tunnel_decap_group_key, attr_list_in_app_db, "SWSS_RC_UNIMPLEMENTED",
            "[OrchAgent] SWSS_RC_UNIMPLEMENTED")

        # Delete the tunnel decap group.
        self._p4rt_tunnel_decap_wrapper.remove_app_db_entry(
            tunnel_decap_group_key)
        self._p4rt_tunnel_decap_wrapper.verify_response(
            tunnel_decap_group_key, [], "SWSS_RC_SUCCESS")

    def test_TunnelDecapGroupDeleteBeforeAddFails(self, dvs, testlog):
        # Initialize database connectors
        self._set_up(dvs)

        ipv6 = "2001:db8:3c4d:15::&ffff:ffff:ffff:ffff::"

        tunnel_decap_group_key = self._p4rt_tunnel_decap_wrapper.generate_app_db_key(
            ipv6)

        # Remove tunnel decap group fails
        self._p4rt_tunnel_decap_wrapper.remove_app_db_entry(
            tunnel_decap_group_key)
        self._p4rt_tunnel_decap_wrapper.verify_response(
            tunnel_decap_group_key, [], "SWSS_RC_NOT_FOUND",
            "[OrchAgent] Ipv6 tunnel termination table entry with key "
            "'dst_ipv6_ip=2001:db8:3c4d:15:::dst_ipv6_mask=ffff:ffff:ffff:ffff::' "
            "does not exist in tunnel decap group manager")
