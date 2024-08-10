from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.eni_route_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_group_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *
from google.protobuf.json_format import ParseDict, MessageToDict

from dash_utils.dash_db import dash_db

import time
import uuid
import ipaddress
import socket
import pytest

from dash_configs import *


class DashBaseTest(object):

    @pytest.fixture
    def setup_appliance(self, dash_db):
        pb = ParseDict(APPLIANCE, Appliance())
        dash_db.create_appliance(APPLIANCE_ID, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_appliance(APPLIANCE_ID)

    @pytest.fixture
    def setup_vnet(self, dash_db, setup_appliance):
        pb = ParseDict(VNET, Vnet())
        dash_db.create_vnet(VNET1, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_vnet(VNET1)

    @pytest.fixture
    def setup_eni(self, dash_db, setup_vnet):
        pb = ParseDict(ENI, Eni())
        dash_db.create_eni(ENI_ID, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_eni(ENI_ID)

    @pytest.fixture
    def setup_routing_type(self, dash_db):
        pb = ParseDict(ROUTING_TYPE_VNET_ENCAP, RouteType())
        dash_db.create_routing_type(VNET_ENCAP, {"pb": pb.SerializeToString()})
        pb = ParseDict(ROUTING_TYPE_PL, RouteType())
        dash_db.create_routing_type(PRIVATELINK, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_routing_type(VNET_ENCAP)
        dash_db.remove_routing_type(PRIVATELINK)

    @pytest.fixture
    def setup_vnet_mapping(self, dash_db, setup_eni):
        pb = ParseDict(VNET_MAPPING, VnetMapping())
        dash_db.create_vnet_mapping(VNET1, VNET_MAP_IP1, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_vnet_mapping(VNET1, VNET_MAP_IP1)

    @pytest.fixture
    def setup_route_group(self, dash_db):
        pb = ParseDict(ROUTE_GROUP, RouteGroup())
        dash_db.create_route_group(ROUTE_GROUP1, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_route_group(ROUTE_GROUP1)

    @pytest.fixture
    def setup_eni_route(self, dash_db, setup_eni, setup_route_group):
        pb = ParseDict(ENI_ROUTE, EniRoute())
        dash_db.create_eni_route(ENI_ID, ROUTE_GROUP1, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_eni_route(ENI_ID, ROUTE_GROUP1)

    @pytest.fixture
    def setup_route(self, dash_db, setup_vnet_mapping, setup_route_group):
        pb = ParseDict(ROUTE, Route())
        dash_db.create_outbound_routing(ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX, {"pb": pb.SerializeToString()})

        yield

        dash_db.remove_outbound_routing(ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX)