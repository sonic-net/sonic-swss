from swsscommon import swsscommon

import util
import json

class P4RtMirrorSessionWrapper(util.DBInterface):
  """Interface to interact with APP DB and ASIC DB tables for P4RT mirror session object."""

  # database and SAI constants
  APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
  TBL_NAME = swsscommon.APP_P4RT_MIRROR_SESSION_TABLE_NAME
  ACTION = "action"
  PORT = "port"
  SRC_IP = "src_ip"
  DST_IP = "dst_ip"
  SRC_MAC = "src_mac"
  DST_MAC = "dst_mac"
  TTL = "ttl"
  TOS = "tos"

  ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION"
  SAI_MIRROR_SESSION_ATTR_MONITOR_PORT = "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT"
  SAI_MIRROR_SESSION_ATTR_TYPE = "SAI_MIRROR_SESSION_ATTR_TYPE"
  SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE = "SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE"
  SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION = "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION"
  SAI_MIRROR_SESSION_ATTR_TOS = "SAI_MIRROR_SESSION_ATTR_TOS"
  SAI_MIRROR_SESSION_ATTR_TTL = "SAI_MIRROR_SESSION_ATTR_TTL"
  SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS = "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS"
  SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS = "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS"
  SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS = "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS"
  SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS = "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS"
  SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE = "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE"

  def generate_app_db_key(self, mirror_session_id):
    d = {}
    d[util.prepend_match_field("mirror_session_id")] = mirror_session_id
    key = json.dumps(d, separators=(",", ":"))
    return self.TBL_NAME + ":" + key

class TestP4RTMirror(object):
    def _set_up(self, dvs):
        self._p4rt_mirror_session_wrapper = P4RtMirrorSessionWrapper()
        self._p4rt_mirror_session_wrapper.set_up_databases(dvs)
        self._response_consumer = swsscommon.NotificationConsumer(
            self._p4rt_mirror_session_wrapper.appl_state_db, "APPL_DB_P4RT_TABLE_RESPONSE_CHANNEL")

    def test_MirrorSessionAddModifyAndDelete(self, dvs, testlog):
        # Initialize database connectors
        self._set_up(dvs)

        # Maintain list of original Application and ASIC DB entries before adding
        # new mirror session
        original_appl_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.appl_db,
            self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_mirror_session_wrapper.TBL_NAME)
        original_appl_state_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.appl_state_db,
            self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_mirror_session_wrapper.TBL_NAME)
        original_asic_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.asic_db, self._p4rt_mirror_session_wrapper.ASIC_DB_TBL_NAME)

        # 1. Create mirror session
        mirror_session_id = "mirror_session1"
        action = "mirror_as_ipv4_erspan"
        port = "Ethernet8"
        src_ip = "10.206.196.31"
        dst_ip = "172.20.0.203"
        src_mac = "00:02:03:04:05:06"
        dst_mac = "00:1A:11:17:5F:80"
        ttl = "0x40"
        tos = "0x00"

        attr_list_in_app_db = [(self._p4rt_mirror_session_wrapper.ACTION, action),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.PORT), port),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.SRC_IP), src_ip),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.DST_IP), dst_ip),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.SRC_MAC), src_mac),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.DST_MAC), dst_mac),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.TTL), ttl),
                     (util.prepend_param_field(self._p4rt_mirror_session_wrapper.TOS), tos)]
        mirror_session_key = self._p4rt_mirror_session_wrapper.generate_app_db_key(
            mirror_session_id)
        self._p4rt_mirror_session_wrapper.set_app_db_entry(
            mirror_session_key, attr_list_in_app_db)
        util.verify_response(
            self._response_consumer, mirror_session_key, attr_list_in_app_db, "SWSS_RC_SUCCESS")

        # Query application database for mirror entries
        appl_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.appl_db,
            self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_mirror_session_wrapper.TBL_NAME)
        assert len(appl_mirror_entries) == len(original_appl_mirror_entries) + 1

        # Query application database for newly created mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.appl_db,
                                     self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME,
                                     mirror_session_key)
        assert status == True
        util.verify_attr(fvs, attr_list_in_app_db)

        # Query application state database for mirror entries
        appl_state_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.appl_state_db,
            self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_mirror_session_wrapper.TBL_NAME)
        assert len(appl_state_mirror_entries) == len(original_appl_state_mirror_entries) + 1

        # Query application state database for newly created mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.appl_state_db,
                                     self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME,
                                     mirror_session_key)
        assert status == True
        util.verify_attr(fvs, attr_list_in_app_db)

        # Query ASIC database for mirror entries
        asic_mirror_entries = util.get_keys(self._p4rt_mirror_session_wrapper.asic_db,
                                      self._p4rt_mirror_session_wrapper.ASIC_DB_TBL_NAME)
        assert len(asic_mirror_entries) == len(original_asic_mirror_entries) + 1

        # Query ASIC database for newly created mirror key
        asic_db_key = None
        for key in asic_mirror_entries:
            # Get newly created entry
            if key not in original_asic_mirror_entries:
                asic_db_key = key
                break
        assert asic_db_key is not None
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.asic_db,
                                     self._p4rt_mirror_session_wrapper.ASIC_DB_TBL_NAME,
                                     asic_db_key)
        assert status == True

        # Get oid of Ethernet8
        port_oid = util.get_port_oid_by_name(dvs, port)
        assert port_oid != None

        expected_attr_list_in_asic_db = [
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_MONITOR_PORT, port_oid),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_TYPE, "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE, "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION, "4"),  # MIRROR_SESSION_DEFAULT_IP_HDR_VER
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_TOS, "0"),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_TTL, "64"),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS, src_ip),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS, dst_ip),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS, src_mac),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS, dst_mac),
          (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE, "35006")  # GRE_PROTOCOL_ERSPAN 0x88be
        ]
        util.verify_attr(fvs, expected_attr_list_in_asic_db)

        # 2. Modify the existing mirror session.
        new_dst_mac = "00:1A:11:17:5F:FF"
        attr_list_in_app_db[5] = (util.prepend_param_field(self._p4rt_mirror_session_wrapper.DST_MAC), new_dst_mac)
        self._p4rt_mirror_session_wrapper.set_app_db_entry(
            mirror_session_key, attr_list_in_app_db)
        util.verify_response(
            self._response_consumer, mirror_session_key, attr_list_in_app_db, "SWSS_RC_SUCCESS")

        # Query application database for the modified mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.appl_db,
                                     self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME,
                                     mirror_session_key)
        assert status == True
        util.verify_attr(fvs, attr_list_in_app_db)

        # Query application state database for the modified mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.appl_state_db,
                                     self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME,
                                     mirror_session_key)
        assert status == True
        util.verify_attr(fvs, attr_list_in_app_db)

        # Query ASIC DB about the modified mirror session.
        expected_attr_list_in_asic_db[9] = (self._p4rt_mirror_session_wrapper.SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS, new_dst_mac)
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.asic_db,
                                     self._p4rt_mirror_session_wrapper.ASIC_DB_TBL_NAME,
                                     asic_db_key)
        assert status == True
        util.verify_attr(fvs, expected_attr_list_in_asic_db)

        # 3. Delete the mirror session.
        self._p4rt_mirror_session_wrapper.remove_app_db_entry(
            mirror_session_key)
        util.verify_response(
            self._response_consumer, mirror_session_key, [], "SWSS_RC_SUCCESS")

        # Query application database for mirror entries
        appl_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.appl_db,
            self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_mirror_session_wrapper.TBL_NAME)
        assert len(appl_mirror_entries) == len(original_appl_mirror_entries)

        # Query application database for the deleted mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.appl_db,
                                     self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME,
                                     mirror_session_key)
        assert status == False

        # Query application state database for mirror entries
        appl_state_mirror_entries = util.get_keys(
            self._p4rt_mirror_session_wrapper.appl_state_db,
            self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME + ":" + self._p4rt_mirror_session_wrapper.TBL_NAME)
        assert len(appl_state_mirror_entries) == len(original_appl_state_mirror_entries)

        # Query application state database for the deleted mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.appl_state_db,
                                     self._p4rt_mirror_session_wrapper.APP_DB_TBL_NAME,
                                     mirror_session_key)
        assert status == False

        # Query ASIC database for mirror entries
        asic_mirror_entries = util.get_keys(self._p4rt_mirror_session_wrapper.asic_db,
                                      self._p4rt_mirror_session_wrapper.ASIC_DB_TBL_NAME)
        assert len(asic_mirror_entries) == len(original_asic_mirror_entries)

        # Query ASIC state database for the deleted mirror key
        (status, fvs) = util.get_key(self._p4rt_mirror_session_wrapper.asic_db,
                                     self._p4rt_mirror_session_wrapper.ASIC_DB_TBL_NAME,
                                     asic_db_key)
        assert status == False
