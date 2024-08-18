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

from dash_utils.dash_configs import *

def create_appliance(dash_db, appliance_id, appliance_config):
    pb = ParseDict(appliance_config, Appliance())
    dash_db.create_appliance(appliance_id, {"pb": pb.SerializeToString()})

def remove_appliance(dash_db, appliance_id):
    dash_db.remove_appliance(appliance_id)

def create_vnet(dash_db, vnet, vnet_config):
    pb = ParseDict(vnet_config, Vnet())
    dash_db.create_vnet(vnet, {"pb": pb.SerializeToString()})

def remove_vnet(dash_db, vnet):
    dash_db.remove_vnet(vnet)

def create_eni(dash_db, eni, eni_config):
    pb = ParseDict(eni_config, Eni())
    dash_db.create_eni(eni, {"pb": pb.SerializeToString()})

def remove_eni(dash_db, eni):
    dash_db.remove_eni(eni)

def create_routing_type(dash_db, routing_type, routing_type_config):
    pb = ParseDict(routing_type_config, RouteType())
    dash_db.create_routing_type(routing_type, {"pb": pb.SerializeToString()})

def remove_routing_type(dash_db, routing_type):
    dash_db.remove_routing_type(routing_type)

def create_vnet_mapping(dash_db, vnet, ip, vnet_mapping_config):
    pb = ParseDict(vnet_mapping_config, VnetMapping())
    dash_db.create_vnet_mapping(vnet, ip, {"pb": pb.SerializeToString()})

def remove_vnet_mapping(dash_db, vnet, ip):
    dash_db.remove_vnet_mapping(vnet, ip)

def create_route_group(dash_db, route_group, route_group_config):
    pb = ParseDict(route_group_config, RouteGroup())
    dash_db.create_route_group(route_group, {"pb": pb.SerializeToString()})

def remove_route_group(dash_db, route_group):
    dash_db.remove_route_group(route_group)

def create_eni_route(dash_db, eni, eni_route_config):
    pb = ParseDict(eni_route_config, EniRoute())
    dash_db.create_eni_route(eni, {"pb": pb.SerializeToString()})

def remove_eni_route(dash_db, eni):
    dash_db.remove_eni_route(eni)

def create_route(dash_db, route_group, route_prefix, route_config):
    pb = ParseDict(route_config, Route())
    dash_db.create_route(route_group, route_prefix, {"pb": pb.SerializeToString()})

def remove_route(dash_db, route_group, route_prefix):
    dash_db.remove_route(route_group, route_prefix)