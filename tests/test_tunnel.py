import time
import pytest

from swsscommon import swsscommon


def create_fvs(**kwargs):
    return swsscommon.FieldValuePairs(list(kwargs.items()))


class TestTunnelBase(object):
    APP_TUNNEL_DECAP_TABLE_NAME = "TUNNEL_DECAP_TABLE"
    ASIC_TUNNEL_TABLE           = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_TERM_ENTRIES    = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_RIF_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_VRF_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"

    ecn_modes_map = {
        "standard"       : "SAI_TUNNEL_DECAP_ECN_MODE_STANDARD",
        "copy_from_outer": "SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER"
    }

    dscp_modes_map = {
        "pipe"    : "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL",
        "uniform" : "SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL"
    }

    ttl_modes_map = {
        "pipe"    : "SAI_TUNNEL_TTL_MODE_PIPE_MODEL",
        "uniform" : "SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL"
    }


    def check_interface_exists_in_asicdb(self, asicdb, sai_oid):
        if_table = swsscommon.Table(asicdb, self.ASIC_RIF_TABLE)
        status, fvs = if_table.get(sai_oid)
        return status

    def check_vr_exists_in_asicdb(self, asicdb, sai_oid):
        vfr_table = swsscommon.Table(asicdb, self.ASIC_VRF_TABLE)
        status, fvs = vfr_table.get(sai_oid)
        return status

    def check_tunnel_termination_entry_exists_in_asicdb(self, asicdb, tunnel_sai_oid, dst_ips):
        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)

        tunnel_term_entries = tunnel_term_table.getKeys()
        assert len(tunnel_term_entries) == len(dst_ips)

        for term_entry in tunnel_term_entries:
            status, fvs = tunnel_term_table.get(term_entry)

            assert status == True
            assert len(fvs) == 5

            for field, value in fvs:
                if field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID":
                    assert self.check_vr_exists_in_asicdb(asicdb, value)
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE":
                    assert value == "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP"
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE":
                    assert value == "SAI_TUNNEL_TYPE_IPINIP"
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID":
                    assert value == tunnel_sai_oid
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP":
                    assert value in dst_ips
                else:
                    assert False, "Field %s is not tested" % field

    def create_and_test_tunnel(self, db, asicdb, tunnel_name, **kwargs):
        """ Create tunnel and verify all needed enties in ASIC DB exists """

        is_symmetric_tunnel = "src_ip" in kwargs;

        # create tunnel entry in DB
        ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TABLE_NAME)

        fvs = create_fvs(**kwargs)

        ps.set(tunnel_name, fvs)

        # wait till config will be applied
        time.sleep(1)

        # check asic db table
        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)

        tunnels = tunnel_table.getKeys()
        assert len(tunnels) == 1

        tunnel_sai_obj = tunnels[0]

        status, fvs = tunnel_table.get(tunnel_sai_obj)

        assert status == True
        # 6 parameters to check in case of decap tunnel
        # + 1 (SAI_TUNNEL_ATTR_ENCAP_SRC_IP) in case of symmetric tunnel
        assert len(fvs) == 7 if is_symmetric_tunnel else 6

        expected_ecn_mode = self.ecn_modes_map[kwargs["ecn_mode"]]
        expected_dscp_mode = self.dscp_modes_map[kwargs["dscp_mode"]]
        expected_ttl_mode = self.ttl_modes_map[kwargs["ttl_mode"]]

        for field, value in fvs:
            if field == "SAI_TUNNEL_ATTR_TYPE":
                assert value == "SAI_TUNNEL_TYPE_IPINIP"
            elif field == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert value == kwargs["src_ip"]
            elif field == "SAI_TUNNEL_ATTR_DECAP_ECN_MODE":
                assert value == expected_ecn_mode
            elif field == "SAI_TUNNEL_ATTR_DECAP_TTL_MODE":
                assert value == expected_ttl_mode
            elif field == "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE":
                assert value == expected_dscp_mode
            elif field == "SAI_TUNNEL_ATTR_OVERLAY_INTERFACE":
                assert self.check_interface_exists_in_asicdb(asicdb, value)
            elif field == "SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE":
                assert self.check_interface_exists_in_asicdb(asicdb, value)
            else:
                assert False, "Field %s is not tested" % field

        self.check_tunnel_termination_entry_exists_in_asicdb(asicdb, tunnel_sai_obj, kwargs["dst_ip"].split(","))

    def remove_and_test_tunnel(self, db, asicdb, tunnel_name):
        """ Removes tunnel and checks that ASIC db is clear"""

        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)
        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)
        tunnel_app_table = swsscommon.Table(asicdb, self.APP_TUNNEL_DECAP_TABLE_NAME)

        tunnels = tunnel_table.getKeys()
        tunnel_sai_obj = tunnels[0]

        status, fvs = tunnel_table.get(tunnel_sai_obj)

        # get overlay loopback interface oid to check if it is deleted with the tunnel
        overlay_infs_id = {f:v for f,v in fvs}["SAI_TUNNEL_ATTR_OVERLAY_INTERFACE"] 

        ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
        ps.set(tunnel_name, create_fvs(), 'DEL')

        # wait till config will be applied
        time.sleep(1)

        assert len(tunnel_table.getKeys()) == 0
        assert len(tunnel_term_table.getKeys()) == 0
        assert len(tunnel_app_table.getKeys()) == 0
        assert not self.check_interface_exists_in_asicdb(asicdb, overlay_infs_id)


    def cleanup_left_over(self, db, asicdb):
        """ Cleanup APP and ASIC tables """

        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)
        for key in tunnel_table.getKeys():
            tunnel_table._del(key)

        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)
        for key in tunnel_term_table.getKeys():
            tunnel_term_table._del(key)

        tunnel_app_table = swsscommon.Table(asicdb, self.APP_TUNNEL_DECAP_TABLE_NAME)
        for key in tunnel_app_table.getKeys():
            tunnel_table._del(key)


class TestDecapTunnel(TestTunnelBase):
    """ Tests for decap tunnel creation and removal """

    def test_TunnelDecap_v4(self, dvs, testlog):
        """ test IPv4 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, asicdb)

        # create tunnel IPv4 tunnel
        self.create_and_test_tunnel(db, asicdb, tunnel_name="IPINIPv4Decap", tunnel_type="IPINIP",
                                   dst_ip="2.2.2.2,3.3.3.3", dscp_mode="uniform",
                                   ecn_mode="standard", ttl_mode="pipe")
        self.remove_and_test_tunnel(db, asicdb, "IPINIPv4Decap")

    def test_TunnelDecap_v6(self, dvs, testlog):
        """ test IPv6 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, asicdb)

        # create tunnel IPv6 tunnel
        self.create_and_test_tunnel(db, asicdb, tunnel_name="IPINIPv6Decap", tunnel_type="IPINIP",
                                    dst_ip="2::2,3::3", dscp_mode="pipe",
                                    ecn_mode="copy_from_outer", ttl_mode="uniform")
        self.remove_and_test_tunnel(db, asicdb,"IPINIPv6Decap")


class TestSymmetricTunnel(TestTunnelBase):
    """ Tests for symmetric tunnel creation and removal """

    def test_TunnelSymmetric_v4(self, dvs, testlog):
        """ test IPv4 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, asicdb)

        # create tunnel IPv4 tunnel
        self.create_and_test_tunnel(db, asicdb, tunnel_name="IPINIPv4Symmetric", tunnel_type="IPINIP",
                                   src_ip="1.1.1.1",
                                   dst_ip="2.2.2.2,3.3.3.3", dscp_mode="pipe",
                                   ecn_mode="copy_from_outer", ttl_mode="uniform")
        self.remove_and_test_tunnel(db, asicdb, "IPINIPv4Symmetric")

    def test_TunnelSymmetric_v6(self, dvs, testlog):
        """ test IPv6 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, asicdb)

        # create tunnel IPv6 tunnel
        self.create_and_test_tunnel(db, asicdb, tunnel_name="IPINIPv6Symmetric", tunnel_type="IPINIP",
                                    src_ip="1::1",
                                    dst_ip="2::2,3::3", dscp_mode="uniform",
                                    ecn_mode="standard", ttl_mode="pipe")
        self.remove_and_test_tunnel(db, asicdb, "IPINIPv6Symmetric")

