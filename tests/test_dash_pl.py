import base64
import socket
import pytest
import time
import uuid

from ipaddress import ip_address as IP

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *
from google.protobuf.json_format import ParseDict, MessageToDict

from dash_utils.dash_db import dash_db
from dash_utils.dash_base_test import DashBaseTest
from dash_utils.dash_configs import *
from sai_attrs import *

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2


class TestDashPl(DashBaseTest):
    def test_pl_eni_attrs(self, dash_db, setup_route):
        enis = dash_db.asic_eni_table.get_keys()
        assert enis
        eni_attrs = dash_db.asic_eni_table[enis[0]]
        assert SAI_ENI_ATTR_PL_SIP in eni_attrs
        assert eni_attrs[SAI_ENI_ATTR_PL_SIP] == PL_ENCODING_IP
        assert SAI_ENI_ATTR_PL_SIP_MASK in eni_attrs
        actual_mask = IP(eni_attrs[SAI_ENI_ATTR_PL_SIP_MASK])
        assert actual_mask == IP(PL_ENCODING_MASK)
        assert SAI_ENI_ATTR_PL_UNDERLAY_SIP in eni_attrs
        assert eni_attrs[SAI_ENI_ATTR_PL_UNDERLAY_SIP] == PL_UNDERLAY_SIP

    def test_pl_outbound_ca_to_pa_attrs(self, dash_db, setup_route):
        outbound_ca_to_pa_keys = dash_db.asic_dash_outbound_ca_to_pa_table.get_keys()
        assert outbound_ca_to_pa_keys
        outbound_attrs = dash_db.asic_dash_outbound_ca_to_pa_table[outbound_ca_to_pa_keys[0]]

        assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION in outbound_attrs
        assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION] == SAI_OUTBOUND_CA_TO_PA_ENTRY_ACTION_SET_PRIVATE_LINK_MAPPING
        assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP in outbound_attrs
        actual_overlay_sip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP])
        assert actual_overlay_sip == IP(PL_OVERLAY_SIP)
        assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP in outbound_attrs
        actual_overlay_dip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP])
        assert actual_overlay_dip == IP(PL_OVERLAY_DIP)
        assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY in outbound_attrs
        assert int(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY]) == ENCAP_VNI
        assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION in outbound_attrs
        assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION] == SAI_DASH_ENCAPSULATION_NVGRE
