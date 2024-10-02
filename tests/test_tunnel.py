import ipaddress
import time
import pytest

from swsscommon import swsscommon


def create_fvs(**kwargs):
    return swsscommon.FieldValuePairs(list(kwargs.items()))


def convert_fvs_to_dict(fvs):
    return {f:v for f,v in fvs}


class TestTunnelBase(object):
    APPL_DB_SEPARATOR                   = ":"
    STATE_DB_SEPARATOR                  = "|"
    APP_TUNNEL_DECAP_TABLE_NAME         = "TUNNEL_DECAP_TABLE"
    APP_TUNNEL_DECAP_TERM_TABLE_NAME    = "TUNNEL_DECAP_TERM_TABLE"
    STATE_TUNNEL_DECAP_TABLE_NAME       = "TUNNEL_DECAP_TABLE"
    STATE_TUNNEL_DECAP_TERM_TABLE_NAME  = "TUNNEL_DECAP_TERM_TABLE"
    CFG_SUBNET_DECAP_TABLE_NAME         = "SUBNET_DECAP"
    ASIC_TUNNEL_TABLE                   = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_TERM_ENTRIES            = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_RIF_TABLE                      = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_VRF_TABLE                      = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
    ASIC_QOS_MAP_TABLE_KEY              = "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP"
    TUNNEL_QOS_MAP_NAME                 = "AZURE_TUNNEL"
    CONFIG_TUNNEL_TABLE_NAME            = "TUNNEL"
    SAI_NULL_OBJECT_ID                  = 0

    decap_term_type_map = {
        "P2P"   : "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P",
        "P2MP"  : "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP",
        "MP2P"  : "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2P",
        "MP2MP" : "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP"
    }

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

    # Define 2 dummy maps
    DSCP_TO_TC_MAP = {str(i):str(1) for i in range(0, 64)}
    TC_TO_PRIORITY_GROUP_MAP = {str(i):str(i) for i in range(0, 8)}

    def check_interface_exists_in_asicdb(self, asicdb, sai_oid):
        if_table = swsscommon.Table(asicdb, self.ASIC_RIF_TABLE)
        status, fvs = if_table.get(sai_oid)
        return status

    def check_vr_exists_in_asicdb(self, asicdb, sai_oid):
        vfr_table = swsscommon.Table(asicdb, self.ASIC_VRF_TABLE)
        status, fvs = vfr_table.get(sai_oid)
        return status

    def create_and_test_tunnel_decap_terms(self, db, asicdb, statedb, tunnel_name, tunnel_sai_oid,
                                           decap_term_attr_list, skip_decap_term_creation=False,
                                           is_decap_terms_existed=True, subnet_decap_config=None):
        """Create decap terms and verify all needed entries in ASIC DB exists"""
        if not skip_decap_term_creation:
            ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TERM_TABLE_NAME)
            for decap_term_attrs in decap_term_attr_list:
                dst_ip = decap_term_attrs["dst_ip"]
                fvs = create_fvs(**{k:v for k,v in decap_term_attrs.items() if k != "dst_ip"})
                ps.set(tunnel_name + self.APPL_DB_SEPARATOR + dst_ip, fvs)

        time.sleep(1)

        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)

        tunnel_term_entries = tunnel_term_table.getKeys()
        if not is_decap_terms_existed:
            assert len(tunnel_term_entries) == 0
            return
        assert len(tunnel_term_entries) == len(decap_term_attr_list)

        decap_terms = {}
        for decap_term_attrs in decap_term_attr_list:
            dst_ip = ipaddress.ip_network(decap_term_attrs["dst_ip"])
            decap_terms[(str(dst_ip.network_address), str(dst_ip.netmask))] = decap_term_attrs

        for term_entry in tunnel_term_entries:
            status, fvs = tunnel_term_table.get(term_entry)
            term_attrs = convert_fvs_to_dict(fvs)
            dst_ip = term_attrs["SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP"]
            if "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK" in term_attrs:
                dst_ip_mask = term_attrs["SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK"]
            else:
                if ipaddress.ip_address(dst_ip).version == 4:
                    dst_ip_mask = "255.255.255.255"
                else:
                    dst_ip_mask = "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"

            decap_term = decap_terms[(dst_ip, dst_ip_mask)]
            dst_ip_str = decap_term["dst_ip"]

            decap_term_type_str = decap_term.get("term_type", "P2MP")
            if decap_term_type_str == "P2MP":
                expected_len = 5
            elif decap_term_type_str == "P2P":
                expected_len = 6
            elif decap_term_type_str == "MP2P":
                expected_len = 7
            elif decap_term_type_str == "MP2MP":
                expected_len = 8

            assert status == True
            assert len(fvs) == expected_len

            decap_term_type = self.decap_term_type_map[decap_term_type_str]
            for field, value in term_attrs.items():
                if field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID":
                    assert self.check_vr_exists_in_asicdb(asicdb, value)
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE":
                    assert value == decap_term_type
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE":
                    assert value == "SAI_TUNNEL_TYPE_IPINIP"
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID":
                    assert value == tunnel_sai_oid
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP":
                    assert value == str(ipaddress.ip_network(dst_ip_str).network_address)
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK":
                    assert value == str(ipaddress.ip_network(dst_ip_str).netmask)
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP":
                    if subnet_decap_config:
                        src_ip_asic = ipaddress.ip_address(value)
                        if src_ip_asic.version == 4:
                            expected_value = str(ipaddress.ip_network(subnet_decap_config["src_ip"]).network_address)
                        else:
                            expected_value = str(ipaddress.ip_network(subnet_decap_config["src_ip_v6"]).network_address)
                    else:
                        expected_value = str(ipaddress.ip_network(decap_term["src_ip"]).network_address)
                    assert value == expected_value
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK":
                    if subnet_decap_config:
                        src_ip_mask_asic = ipaddress.ip_address(value)
                        if src_ip_mask_asic.version == 4:
                            expected_value = str(ipaddress.ip_network(subnet_decap_config["src_ip"]).netmask)
                        else:
                            expected_value = str(ipaddress.ip_network(subnet_decap_config["src_ip_v6"]).netmask)
                    else:
                        expected_value = str(ipaddress.ip_network(decap_term["src_ip"]).netmask)
                    assert value == expected_value
                else:
                    assert False, "Field %s is not tested" % field

        tunnel_decap_term_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TERM_TABLE_NAME)

        tunnel_term_state_entries = tunnel_decap_term_state_table.getKeys()
        for term_entry in tunnel_term_state_entries:
            status, fvs = tunnel_decap_term_state_table.get(term_entry)
            tunnel_name, dst_ip_str = term_entry.split(self.STATE_DB_SEPARATOR)
            dst_ip = ipaddress.ip_network(dst_ip_str)

            assert (str(dst_ip.network_address), str(dst_ip.netmask)) in decap_terms
            assert status == True

            decap_term = decap_terms[(str(dst_ip.network_address), str(dst_ip.netmask))]
            assert dst_ip_str == decap_term["dst_ip"]
            for field, value in fvs:
                if field == "src_ip":
                    if subnet_decap_config:
                        if dst_ip.version == 4:
                            expected_value = subnet_decap_config["src_ip"]
                        else:
                            expected_value = subnet_decap_config["src_ip_v6"]
                    else:
                        expected_value = decap_term["src_ip"]
                    assert value == expected_value
                elif field == "term_type":
                    assert value == decap_term.get("term_type", "P2MP")
                elif field == "subnet_type":
                    assert value == decap_term["subnet_type"]
                else:
                    assert False, "Field %s is not tested" % field

    def remove_and_test_tunnel_decap_terms(self, db, asicdb, statedb, tunnel_name, decap_term_attr_list):
        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)
        tunnel_decap_term_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TERM_TABLE_NAME)

        dst_ips = {decap_term_attrs["dst_ip"] for decap_term_attrs in decap_term_attr_list}
        ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TERM_TABLE_NAME)
        for dst_ip in dst_ips:
            ps._del(tunnel_name + self.APPL_DB_SEPARATOR + dst_ip)

        time.sleep(1)

        assert len(tunnel_term_table.getKeys()) == 0
        assert len(tunnel_decap_term_state_table.getKeys()) == 0

    def create_and_test_tunnel(self, db, asicdb, statedb, tunnel_name, **kwargs):
        """ Create tunnel and verify all needed entries in ASIC DB exists """

        is_symmetric_tunnel = "src_ip" in kwargs

        decap_dscp_to_tc_map_oid = None
        decap_tc_to_pg_map_oid = None
        skip_tunnel_creation = False
        is_tunnel_existed = True

        if "decap_dscp_to_tc_map_oid" in kwargs:
            decap_dscp_to_tc_map_oid = kwargs.pop("decap_dscp_to_tc_map_oid")

        if "decap_tc_to_pg_map_oid" in kwargs:
            decap_tc_to_pg_map_oid = kwargs.pop("decap_tc_to_pg_map_oid")

        if "skip_tunnel_creation" in kwargs:
            skip_tunnel_creation = kwargs.pop("skip_tunnel_creation")
        if "is_tunnel_existed" in kwargs:
            is_tunnel_existed = kwargs.pop("is_tunnel_existed")

        if not skip_tunnel_creation:
            fvs = create_fvs(**kwargs)
            # create tunnel entry in DB
            ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
            ps.set(tunnel_name, fvs)

        # wait till config will be applied
        time.sleep(1)

        # check asic db table
        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)

        tunnels = tunnel_table.getKeys()
        if not is_tunnel_existed:
            assert len(tunnels) == 0
            return self.SAI_NULL_OBJECT_ID

        assert len(tunnels) == 1

        tunnel_sai_obj = tunnels[0]

        status, fvs = tunnel_table.get(tunnel_sai_obj)

        assert status == True
        # 6 parameters to check in case of decap tunnel
        # + 1 (SAI_TUNNEL_ATTR_ENCAP_SRC_IP) in case of symmetric tunnel
        expected_len = 7 if is_symmetric_tunnel else 6

        expected_ecn_mode = self.ecn_modes_map[kwargs["ecn_mode"]]
        expected_dscp_mode = self.dscp_modes_map[kwargs["dscp_mode"]]
        expected_ttl_mode = self.ttl_modes_map[kwargs["ttl_mode"]]

        if decap_dscp_to_tc_map_oid:
            expected_len += 1
        if decap_tc_to_pg_map_oid:
            expected_len += 1

        assert len(fvs) == expected_len

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
            elif field == "SAI_TUNNEL_ATTR_DECAP_QOS_DSCP_TO_TC_MAP":
                assert value == decap_dscp_to_tc_map_oid
            elif field == "SAI_TUNNEL_ATTR_DECAP_QOS_TC_TO_PRIORITY_GROUP_MAP":
                assert value == decap_tc_to_pg_map_oid
            else:
                assert False, "Field %s is not tested" % field

        tunnel_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TABLE_NAME)

        tunnels = tunnel_state_table.getKeys()
        for tunnel in tunnels:
            status, fvs = tunnel_state_table.get(tunnel)
            assert status == True

            for field, value in fvs:
                if field == "tunnel_type":
                    assert value == "IPINIP"
                elif field == "dscp_mode":
                    assert value == kwargs["dscp_mode"]
                elif field == "ecn_mode":
                    assert value == kwargs["ecn_mode"]
                elif field == "ttl_mode":
                    assert value == kwargs["ttl_mode"]
                elif field == "encap_ecn_mode":
                    assert value == kwargs["encap_ecn_mode"]
                else:
                    assert False, "Field %s is not tested" % field

        return tunnel_sai_obj

    def remove_and_test_tunnel(self, db, asicdb, statedb, tunnel_name, skip_validation=False):
        """ Removes tunnel and checks that ASIC db is clear"""

        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)
        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)
        tunnel_app_table = swsscommon.Table(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
        tunnel_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TABLE_NAME)
        tunnel_decap_term_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TERM_TABLE_NAME)

        tunnels = tunnel_table.getKeys()
        tunnel_sai_obj = tunnels[0]

        status, fvs = tunnel_table.get(tunnel_sai_obj)

        # get overlay loopback interface oid to check if it is deleted with the tunnel
        overlay_infs_id = {f:v for f,v in fvs}["SAI_TUNNEL_ATTR_OVERLAY_INTERFACE"]

        ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
        ps._del(tunnel_name)

        if skip_validation:
            return

        # wait till config will be applied
        time.sleep(1)

        assert len(tunnel_table.getKeys()) == 0
        assert len(tunnel_term_table.getKeys()) == 0
        assert len(tunnel_app_table.getKeys()) == 0
        assert len(tunnel_state_table.getKeys()) == 0
        assert len(tunnel_decap_term_state_table.getKeys()) == 0
        assert not self.check_interface_exists_in_asicdb(asicdb, overlay_infs_id)

    def add_qos_map(self, configdb, asicdb, qos_map_type_name, qos_map_name, qos_map):
        """ Add qos map for testing"""
        qos_table = swsscommon.Table(asicdb, self.ASIC_QOS_MAP_TABLE_KEY)
        current_oids = qos_table.getKeys()

        # Apply QoS map to config db
        table = swsscommon.Table(configdb, qos_map_type_name)
        fvs = swsscommon.FieldValuePairs(list(qos_map.items()))
        table.set(qos_map_name, fvs)
        time.sleep(1)

        diff = set(qos_table.getKeys()) - set(current_oids)
        assert len(diff) == 1
        oid = diff.pop()
        return oid

    def remove_qos_map(self, configdb, qos_map_type_name, qos_map_name):
        """ Remove the testing qos map"""
        table = swsscommon.Table(configdb, qos_map_type_name)
        table._del(qos_map_name)

    def cleanup_left_over(self, db, statedb, asicdb):
        """ Cleanup APP and ASIC tables """

        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)
        for key in tunnel_table.getKeys():
            tunnel_table._del(key)

        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)
        for key in tunnel_term_table.getKeys():
            tunnel_term_table._del(key)

        tunnel_decap_term_app_table = swsscommon.Table(db, self.APP_TUNNEL_DECAP_TERM_TABLE_NAME)
        for key in tunnel_decap_term_app_table.getKeys():
            tunnel_decap_term_app_table._del(key)

        tunnel_app_table = swsscommon.Table(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
        for key in tunnel_app_table.getKeys():
            tunnel_app_table._del(key)

        tunnel_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TABLE_NAME)
        for key in tunnel_state_table.getKeys():
            tunnel_state_table._del(key)

        tunnel_decap_term_state_table = swsscommon.Table(statedb, self.STATE_TUNNEL_DECAP_TERM_TABLE_NAME)
        for key in tunnel_decap_term_state_table.getKeys():
            tunnel_decap_term_state_table._del(key)

class TestDecapTunnel(TestTunnelBase):
    """ Tests for decap tunnel creation and removal """

    def test_TunnelDecap_v4(self, dvs, testlog):
        """ test IPv4 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        decap_terms = [
            {"dst_ip": "2.2.2.2", "term_type": "P2MP"},
            {"dst_ip": "3.3.3.3", "term_type": "P2MP"},
            {"dst_ip": "4.4.4.4", "src_ip": "5.5.5.5", "term_type": "P2P"},
            {"dst_ip": "192.168.0.0/24", "src_ip": "10.10.10.0/24", "term_type": "MP2MP"}
        ]
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIPv4Decap", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap",
            tunnel_sai_oid, decap_terms
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv4Decap")

    def test_TunnelDecap_v6(self, dvs, testlog):
        """ test IPv6 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        decap_terms = [
            {"dst_ip": "2::2", "term_type": "P2MP"},
            {"dst_ip": "3::3", "term_type": "P2MP"},
            {"dst_ip": "4::4", "src_ip": "5::5", "term_type": "P2P"},
            {"dst_ip": "2001:db8::/32", "src_ip": "2002:db8::/32", "term_type": "MP2MP"}
        ]
        # create tunnel IPv6 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIPv6Decap", tunnel_type="IPINIP",
            dscp_mode="pipe", ecn_mode="copy_from_outer", ttl_mode="uniform"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv6Decap",
            tunnel_sai_oid, decap_terms
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv6Decap", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv6Decap")

    def test_TunnelDecap_Invalid_Decap_Term_Attribute(self, dvs, testlog):
        """ test IPv4 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        decap_terms = [
            {"dst_ip": "3.3.3.3", "term_type": "P2P"},
            {"dst_ip": "4.4.4.4", "term_type": "MP2MP"}
        ]
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIPv4Decap", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap",
            tunnel_sai_oid, decap_terms,
            is_decap_terms_existed=False
        )

        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv4Decap")

    def test_TunnelDecap_Remove_Tunnel_First(self, dvs, testlog):
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        decap_terms = [
            {"dst_ip": "2.2.2.2", "term_type": "P2MP"},
            {"dst_ip": "3.3.3.3", "term_type": "P2MP"},
            {"dst_ip": "4.4.4.4", "src_ip": "5.5.5.5", "term_type": "P2P"},
            {"dst_ip": "192.168.0.0/24", "src_ip": "10.10.10.0/24", "term_type": "MP2MP"}
        ]
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIPv4Decap", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap",
            tunnel_sai_oid, decap_terms
        )

        # the removal of tunnel with decap terms will fail
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv4Decap", skip_validation=True)
        # validate the tunnel and decap terms are still existed
        self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIPv4Decap", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe",
            skip_tunnel_creation=True
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap",
            tunnel_sai_oid, decap_terms,
            skip_decap_term_creation=True
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap", decap_terms
        )
        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)
        tunnels = tunnel_table.getKeys()
        assert len(tunnels) == 0

    def test_TunnelDecap_Add_Decap_Term_First(self, dvs, testlog):
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        decap_terms = [
            {"dst_ip": "2.2.2.2", "term_type": "P2MP"},
            {"dst_ip": "3.3.3.3", "term_type": "P2MP"},
            {"dst_ip": "4.4.4.4", "src_ip": "5.5.5.5", "term_type": "P2P"},
            {"dst_ip": "192.168.0.0/24", "src_ip": "10.10.10.0/24", "term_type": "MP2MP"}
        ]
        # create decap terms of not-existed tunnel
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap",
            self.SAI_NULL_OBJECT_ID, decap_terms, is_decap_terms_existed=False
        )
        # remove decap terms of not-existed tunnel
        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap", decap_terms[:1]
        )
        # create tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIPv4Decap", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        # verify the decap terms are created
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap",
            tunnel_sai_oid, decap_terms[1:], skip_decap_term_creation=True
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Decap", decap_terms[1:]
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv4Decap")

    def test_TunnelDecap_MuxTunnel(self, dvs, testlog):
        """ Test MuxTunnel creation. """
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        dscp_to_tc_map_oid = self.add_qos_map(configdb, asicdb, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.DSCP_TO_TC_MAP)
        tc_to_pg_map_oid = self.add_qos_map(configdb, asicdb, swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.TC_TO_PRIORITY_GROUP_MAP)

        # Create MuxTunnel0 with QoS remapping attributes
        params = {
            "tunnel_type": "IPINIP",
            "src_ip": "1.1.1.1",
            "dscp_mode": "pipe",
            "ecn_mode": "copy_from_outer",
            "ttl_mode": "uniform",
            "decap_dscp_to_tc_map": "AZURE_TUNNEL",
            "decap_dscp_to_tc_map_oid": dscp_to_tc_map_oid,
            "decap_tc_to_pg_map": "AZURE_TUNNEL",
            "decap_tc_to_pg_map_oid": tc_to_pg_map_oid
        }
        decap_terms = [{"dst_ip": "1.1.1.2", "src_ip": "1.1.1.1", "term_type": "P2P"}]
        tunnel_sai_oid = self.create_and_test_tunnel(db, asicdb, statedb, tunnel_name="MuxTunnel0", **params)
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "MuxTunnel0",
            tunnel_sai_oid, decap_terms
        )
        # Remove Tunnel first
        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "MuxTunnel0", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "MuxTunnel0")

        self.remove_qos_map(configdb, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME)
        self.remove_qos_map(configdb, swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME)

    def test_TunnelDecap_MuxTunnel_with_retry(self, dvs, testlog):
        """ Test MuxTunnel creation. """
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        # Create MuxTunnel0 with QoS remapping attributes
        params = {
            "tunnel_type": "IPINIP",
            "src_ip": "1.1.1.1",
            "dscp_mode": "pipe",
            "ecn_mode": "copy_from_outer",
            "ttl_mode": "uniform",
            "decap_dscp_to_tc_map": "AZURE_TUNNEL",
            "decap_tc_to_pg_map": "AZURE_TUNNEL",
        }
        decap_terms = [
            {"dst_ip": "1.1.1.2", "src_ip": "1.1.1.1", "term_type": "P2P"},
        ]
        # Verify tunnel is not created when decap_dscp_to_tc_map/decap_tc_to_pg_map is specified while oid is not ready in qosorch
        self.create_and_test_tunnel(db, asicdb, statedb, tunnel_name="MuxTunnel0", is_tunnel_existed=False, **params)
        # create decap term entry in DB
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "MuxTunnel0",
            self.SAI_NULL_OBJECT_ID, decap_terms, is_decap_terms_existed=False
        )

        #Verify tunneldecaporch creates tunnel when qos map is available
        dscp_to_tc_map_oid = self.add_qos_map(configdb, asicdb, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.DSCP_TO_TC_MAP)
        tc_to_pg_map_oid = self.add_qos_map(configdb, asicdb, swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.TC_TO_PRIORITY_GROUP_MAP)
        params.update({
                "decap_dscp_to_tc_map_oid": dscp_to_tc_map_oid,
                "decap_tc_to_pg_map_oid": tc_to_pg_map_oid,
                "skip_tunnel_creation": True
            })
        tunnel_sai_oid = self.create_and_test_tunnel(db, asicdb, statedb, tunnel_name="MuxTunnel0", **params)
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "MuxTunnel0",
            tunnel_sai_oid, decap_terms, skip_decap_term_creation=True
        )

        # Cleanup
        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "MuxTunnel0", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "MuxTunnel0")
        self.remove_qos_map(configdb, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME, dscp_to_tc_map_oid)
        self.remove_qos_map(configdb, swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, tc_to_pg_map_oid)


class TestSymmetricTunnel(TestTunnelBase):
    """ Tests for symmetric tunnel creation and removal """

    def test_TunnelSymmetric_v4(self, dvs, testlog):
        """ test IPv4 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        # create tunnel IPv4 tunnel
        decap_terms = [
            {"dst_ip": "2.2.2.2", "src_ip": "1.1.1.1", "term_type": "P2P"},
            {"dst_ip": "3.3.3.3", "src_ip": "1.1.1.1", "term_type": "P2P"},
        ]
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, tunnel_name="IPINIPv4Symmetric",
            tunnel_type="IPINIP", src_ip="1.1.1.1", dscp_mode="pipe",
            ecn_mode="copy_from_outer", ttl_mode="uniform"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Symmetric",
            tunnel_sai_oid, decap_terms
        )
        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv4Symmetric", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv4Symmetric")

    def test_TunnelSymmetric_v6(self, dvs, testlog):
        """ test IPv6 tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        # create tunnel IPv6 tunnel
        decap_terms = [
            {"dst_ip": "2::2", "src_ip": "1::1", "term_type": "P2P"},
            {"dst_ip": "3::3", "src_ip": "1::1", "term_type": "P2P"},
        ]
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, tunnel_name="IPINIPv6Symmetric",
            tunnel_type="IPINIP", src_ip="1::1", dscp_mode="uniform",
            ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv6Symmetric",
            tunnel_sai_oid, decap_terms
        )
        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIPv6Symmetric", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIPv6Symmetric")


class TestSubnetDecap(TestTunnelBase):
    """ Tests for subnet decap creation and removal """

    @pytest.fixture
    def setup_subnet_decap(self, dvs):

        def _apply_subnet_decap_config(subnet_decap_config):
            """Apply subnet decap config to CONFIG_DB."""
            fvs = create_fvs(**subnet_decap_config)
            subnet_decap_tbl.set("AZURE", fvs)

        def _cleanup_subnet_decap_config():
            """Cleanup subnet decap config in CONFIG_DB."""
            for key in subnet_decap_tbl.getKeys():
                subnet_decap_tbl._del(key)

        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        subnet_decap_tbl = swsscommon.Table(configdb, self.CFG_SUBNET_DECAP_TABLE_NAME)
        _cleanup_subnet_decap_config()

        yield _apply_subnet_decap_config

        _cleanup_subnet_decap_config()

    def test_SubnetDecap_Enable_Source_IP_Update_v4(self, dvs, testlog, setup_subnet_decap):
        """Test subnet decap source IP update."""
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        decap_terms = [
            {"dst_ip": "192.168.0.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.1.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.2.0/24", "term_type": "MP2MP", "subnet_type": "vlan"}
        ]
        setup_subnet_decap(subnet_decap_config)
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIP_SUBNET", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            subnet_decap_config=subnet_decap_config
        )

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.20.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            skip_decap_term_creation=True,
            subnet_decap_config=subnet_decap_config
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIP_SUBNET")

    def test_SubnetDecap_Enable_Source_IP_Update_v6(self, dvs, testlog, setup_subnet_decap):
        """Test subnet decap source IPv6 update."""
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        decap_terms = [
            {"dst_ip": "fc02:1000::/64", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "fc02:1001::/64", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "fc02:1002::/64", "term_type": "MP2MP", "subnet_type": "vlan"}
        ]
        setup_subnet_decap(subnet_decap_config)
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIP_SUBNET_V6", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET_V6",
            tunnel_sai_oid, decap_terms,
            subnet_decap_config=subnet_decap_config
        )

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba9::/64"
        }
        setup_subnet_decap(subnet_decap_config)
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET_V6",
            tunnel_sai_oid, decap_terms,
            skip_decap_term_creation=True,
            subnet_decap_config=subnet_decap_config
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET_V6", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIP_SUBNET_V6")

    def test_SubnetDecap_Enable_Source_IP_Update_Add_Decap_Term_First_1(self, dvs, testlog, setup_subnet_decap):
        """Test subnet decap source IP update."""
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        decap_terms = [
            {"dst_ip": "192.168.0.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.1.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.2.0/24", "term_type": "MP2MP", "subnet_type": "vlan"}
        ]
        setup_subnet_decap(subnet_decap_config)
        # create decap terms of not-existed tunnel
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            self.SAI_NULL_OBJECT_ID, decap_terms,
            is_decap_terms_existed=False
        )
        # create tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIP_SUBNET", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        # verify the decap terms are created
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            skip_decap_term_creation=True,
            subnet_decap_config=subnet_decap_config
        )

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.20.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            skip_decap_term_creation=True,
            subnet_decap_config=subnet_decap_config
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIP_SUBNET")

    def test_SubnetDecap_Enable_Source_IP_Update_Add_Decap_Term_First_2(self, dvs, testlog, setup_subnet_decap):
        """Test subnet decap source IP update."""
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        decap_terms = [
            {"dst_ip": "192.168.0.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.1.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.2.0/24", "term_type": "MP2MP", "subnet_type": "vlan"}
        ]
        setup_subnet_decap(subnet_decap_config)
        # create decap terms of not-existed tunnel
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            self.SAI_NULL_OBJECT_ID, decap_terms,
            is_decap_terms_existed=False
        )

        # update subnet decap source IP
        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.20.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)

        # create tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIP_SUBNET", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        # verify the decap terms are created with updated source IP
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            skip_decap_term_creation=True,
            subnet_decap_config=subnet_decap_config
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIP_SUBNET")

    def test_SubnetDecap_Disable(self, dvs, testlog, setup_subnet_decap):
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        subnet_decap_config = {
            "status": "disable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        decap_terms = [
            {"dst_ip": "192.168.0.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.1.0/24", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.2.0/24", "term_type": "MP2MP", "subnet_type": "vlan"}
        ]
        setup_subnet_decap(subnet_decap_config)
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIP_SUBNET", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        # subnet decap is disabled, no decap term will be created
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            subnet_decap_config=subnet_decap_config,
            is_decap_terms_existed=False
        )

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.20.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            subnet_decap_config=subnet_decap_config
        )

        self.remove_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET", decap_terms
        )
        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIP_SUBNET")

    def test_SubnetDecap_Invalid_Decap_Term_Attribute(self, dvs, testlog, setup_subnet_decap):
        """Test adding decap terms with invalid attributes."""
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        self.cleanup_left_over(db, statedb, asicdb)

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        decap_terms = [
            {"dst_ip": "192.168.0.0abc", "term_type": "MP2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.1.0/24", "term_type": "MP2MPP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.2.0/24", "term_type": "P2MP", "subnet_type": "vlan"},
            {"dst_ip": "192.168.3.0/24", "term_type": "MP2MP", "subnet_type": "uknown"},
            {"dst_ip": "192.168.4.0/24", "term_type": "MP2MP", "subnet_type": "vlan", "bad_attr": "bad_val"}
        ]
        setup_subnet_decap(subnet_decap_config)
        # create tunnel IPv4 tunnel
        tunnel_sai_oid = self.create_and_test_tunnel(
            db, asicdb, statedb, "IPINIP_SUBNET", tunnel_type="IPINIP",
            dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe"
        )
        self.create_and_test_tunnel_decap_terms(
            db, asicdb, statedb, "IPINIP_SUBNET",
            tunnel_sai_oid, decap_terms,
            subnet_decap_config=subnet_decap_config,
            is_decap_terms_existed=False
        )

        self.remove_and_test_tunnel(db, asicdb, statedb, "IPINIP_SUBNET")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
