import base64
import pytest
import socket
from ipaddress import ip_address as IP
from swsscommon.swsscommon import (
    APP_DASH_TUNNEL_TABLE_NAME,
    APP_DASH_APPLIANCE_TABLE_NAME,
)

import dash_configs as dc
from dash_db import dash_db_module, dash_db, DashDB
from dvslib.sai_utils import assert_sai_attribute_exists
import sai_attrs as sai
import dash_api.route_type_pb2 as rt

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

def get_expected_tunnel_ips(tunnel_config):
    # We expect orchagent to ignore duplicate IPs, so use a set to ensure the expected IPs are unique
    ips = set()
    for endpoint in tunnel_config["endpoints"]:
        if "ipv4" in endpoint:
            ip = IP(socket.ntohl(endpoint["ipv4"]))
        else:
            ip = IP(base64.b64decode(endpoint["ipv6"]))
        ips.add(str(ip))

    return list(ips)


def verify_sai_tunnel_endpoints(
    dash_db, tunnel_oid, expected_ips, prev_member_keys=None
):
    """
    Check if tunnel members and nhops were created correctly.
    If the tunnel has multiple endpoints, we expect one tunnel member and one tunnel nhop per unique endpoint.
    If the tunnel has a single endpoint, we expect no tunnel members or next hops.
    """
    tunnel_member_oids = []
    tunnel_nhop_oids = []
    tunnel_nhop_ips = []
    if len(expected_ips) > 1:
        member_keys = dash_db.wait_for_asic_db_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER",
            min_keys=len(expected_ips),
            old_keys=prev_member_keys,
        )
        assert len(member_keys) == len(expected_ips), \
            f"Expected {len(expected_ips)} tunnel members, but got: {len(member_keys)}"
        for member in member_keys:
            attrs = dash_db.get_asic_db_entry(
                "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER", member
            )
            if attrs[sai.SAI_DASH_TUNNEL_MEMBER_ATTR_DASH_TUNNEL_ID] == tunnel_oid:
                tunnel_member_oids.append(member)
                nhop_oid = attrs[
                    sai.SAI_DASH_TUNNEL_MEMBER_ATTR_DASH_TUNNEL_NEXT_HOP_ID
                ]
                nhop_ip = dash_db.get_asic_db_entry(
                    "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_NEXT_HOP", nhop_oid
                )
                tunnel_nhop_ips.append(nhop_ip[sai.SAI_DASH_TUNNEL_NEXT_HOP_ATTR_DIP])
                tunnel_nhop_oids.append(nhop_oid)

        assert len(tunnel_nhop_oids) == len(expected_ips), \
            f"Expected {len(expected_ips)} tunnel nhops, but got: {len(tunnel_nhop_oids)}"
        assert sorted(tunnel_nhop_ips) == sorted(expected_ips), \
            f"Expected tunnel nhop IPs: {expected_ips}, but got: {tunnel_nhop_ips}"
        assert len(tunnel_member_oids) == len(expected_ips), \
            f"Expected {len(expected_ips)} tunnel members, but got: {len(tunnel_member_oids)}"
    else:
        member_keys = dash_db.wait_for_asic_db_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER",
            min_keys=0,
            old_keys=prev_member_keys,
        )
        assert (len(member_keys) == 0), \
            f"Expected no tunnel members for single endpoint, but got: {len(member_keys)}"

    return tunnel_member_oids, tunnel_nhop_oids


def verify_sai_tunnel(dash_db, tunnel_oid, tunnel_config, prev_member_keys=None):
    tunnel_attrs = dash_db.get_asic_db_entry(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", tunnel_oid
    )
    expected_ips = get_expected_tunnel_ips(tunnel_config)

    if tunnel_config["encap_type"] == rt.EncapType.ENCAP_TYPE_VXLAN:
        assert_sai_attribute_exists(
            sai.SAI_DASH_TUNNEL_ATTR_DASH_ENCAPSULATION,
            tunnel_attrs,
            sai.SAI_DASH_ENCAPSULATION_VXLAN,
        )
    else:
        assert_sai_attribute_exists(
            sai.SAI_DASH_TUNNEL_ATTR_DASH_ENCAPSULATION,
            tunnel_attrs,
            sai.SAI_DASH_ENCAPSULATION_NVGRE,
        )
    assert_sai_attribute_exists(
        sai.SAI_DASH_TUNNEL_ATTR_TUNNEL_KEY, tunnel_attrs, tunnel_config["vni"]
    )
    assert_sai_attribute_exists(
        sai.SAI_DASH_TUNNEL_ATTR_MAX_MEMBER_SIZE,
        tunnel_attrs,
        len(tunnel_config["endpoints"]),
    )
    assert_sai_attribute_exists(sai.SAI_DASH_TUNNEL_ATTR_SIP, tunnel_attrs, dc.SIP)

    if len(expected_ips) == 1:
        assert_sai_attribute_exists(
            sai.SAI_DASH_TUNNEL_ATTR_DIP, tunnel_attrs, expected_ips[0]
        )
    else:
        assert (
            sai.SAI_DASH_TUNNEL_ATTR_DIP not in tunnel_attrs
        ), "DIP attribute should not be present for multiple endpoints"

    return verify_sai_tunnel_endpoints(
        dash_db, tunnel_oid, expected_ips, prev_member_keys
    )


@pytest.fixture(autouse=True)
def common_setup_teardown(dash_db_module: DashDB):
    dash_db_module.set_app_db_entry(
        APP_DASH_APPLIANCE_TABLE_NAME, dc.APPLIANCE_ID, dc.APPLIANCE_CONFIG
    )
    yield
    dash_db_module.remove_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, dc.APPLIANCE_ID)


@pytest.fixture(autouse=True)
def tunnel_cleanup(dvs, dash_db: DashDB):
    yield
    tunnels = [dc.TUNNEL1, dc.TUNNEL2, dc.TUNNEL3, dc.TUNNEL4, dc.TUNNEL5]
    for t in tunnels:
        dash_db.remove_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, t)

    dvs.check_services_ready()


def test_dash_tunnel_single_endpoint(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL1, dc.TUNNEL1_CONFIG)
    tunnels = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL")
    assert len(tunnels) == 1
    verify_sai_tunnel(dash_db, tunnels[0], dc.TUNNEL1_CONFIG)
    dash_db.remove_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL1)
    dash_db.wait_for_asic_db_key_del(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", tunnels[0]
    )


def test_dash_tunnel_duplicate_tunnels(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL1, dc.TUNNEL1_CONFIG)
    tunnels = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL")
    assert len(tunnels) == 1
    verify_sai_tunnel(dash_db, tunnels[0], dc.TUNNEL1_CONFIG)

    dash_db.set_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL1, dc.TUNNEL2_CONFIG)
    new_tunnels = dash_db.wait_for_asic_db_keys(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", min_keys=0, old_keys=tunnels
    )
    assert (
        len(new_tunnels) == 0
    ), f"Expected no new tunnels, but got: {len(new_tunnels)}"
    # The 2nd APP DB write should be rejected,so we expect SAI to still reflect the first config written
    verify_sai_tunnel(dash_db, tunnels[0], dc.TUNNEL1_CONFIG)

    dash_db.remove_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL1)
    dash_db.wait_for_asic_db_key_del(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", tunnels[0]
    )


def test_dash_tunnel_multiple_endpoints(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL2, dc.TUNNEL2_CONFIG)
    tunnels = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL")
    assert len(tunnels) == 1
    verify_sai_tunnel(
        dash_db, tunnels[0], dc.TUNNEL2_CONFIG
    )

    dash_db.remove_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL2)
    dash_db.wait_for_asic_db_key_del(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", tunnels[0]
    )


def test_dash_tunnel_duplicate_endpoints(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL5, dc.TUNNEL5_CONFIG)
    tunnel_oids = dash_db.wait_for_asic_db_keys(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL"
    )
    assert len(tunnel_oids) == 1
    tunnel_members, tunnel_nhops = verify_sai_tunnel(
        dash_db, tunnel_oids[0], dc.TUNNEL5_CONFIG
    )

    dash_db.remove_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, dc.TUNNEL5)
    for member in tunnel_members:
        dash_db.wait_for_asic_db_key_del(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER", member
        )
    for nhop in tunnel_nhops:
        dash_db.wait_for_asic_db_key_del(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_NEXT_HOP", nhop
        )
    dash_db.wait_for_asic_db_key_del(
        "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", tunnel_oids[0]
    )


def test_dash_multi_tunnel(dash_db: DashDB):
    prev_tunnels, prev_members, prev_nhops = [], [], []
    tunnel_configs = [
        (dc.TUNNEL1, dc.TUNNEL1_CONFIG),
        (dc.TUNNEL2, dc.TUNNEL2_CONFIG),
        (dc.TUNNEL3, dc.TUNNEL3_CONFIG),
        (dc.TUNNEL4, dc.TUNNEL4_CONFIG),
    ]

    for tunnel_name, tunnel_config in tunnel_configs:
        dash_db.set_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, tunnel_name, tunnel_config)
        tunnel_oids = dash_db.wait_for_asic_db_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", old_keys=prev_tunnels
        )
        assert len(tunnel_oids) == 1
        member_oids, nhop_oids = verify_sai_tunnel(
            dash_db, tunnel_oids[0], tunnel_config, prev_members
        )
        prev_tunnels += tunnel_oids
        prev_members += member_oids
        prev_nhops += nhop_oids

    for tunnel_name, _ in tunnel_configs:
        dash_db.remove_app_db_entry(APP_DASH_TUNNEL_TABLE_NAME, tunnel_name)

    for member in prev_members:
        dash_db.wait_for_asic_db_key_del(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER", member
        )
    for nhop in prev_nhops:
        dash_db.wait_for_asic_db_key_del(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL_NEXT_HOP", nhop
        )
    for tunnel in prev_tunnels:
        dash_db.wait_for_asic_db_key_del(
            "ASIC_STATE:SAI_OBJECT_TYPE_DASH_TUNNEL", tunnel
        )
