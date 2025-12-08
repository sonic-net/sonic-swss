from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result
import typing
import pytest
import time

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.eni_route_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_group_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.meter_policy_pb2 import *
from dash_api.meter_rule_pb2 import *
from dash_api.tunnel_pb2 import *
from dash_api.types_pb2 import *
from google.protobuf.json_format import ParseDict
from google.protobuf.message import Message

ASIC_DASH_APPLIANCE_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_DASH_APPLIANCE"
ASIC_DIRECTION_LOOKUP_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_DIRECTION_LOOKUP_ENTRY"
ASIC_VIP_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_VIP_ENTRY"
ASIC_VNET_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_VNET"
ASIC_ENI_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ENI"
ASIC_ENI_ETHER_ADDR_MAP_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ENI_ETHER_ADDRESS_MAP_ENTRY"
ASIC_OUTBOUND_CA_TO_PA_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_CA_TO_PA_ENTRY"
ASIC_PA_VALIDATION_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_PA_VALIDATION_ENTRY"
ASIC_OUTBOUND_ROUTING_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY"
ASIC_INBOUND_ROUTING_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_INBOUND_ROUTING_ENTRY"
ASIC_OUTBOUND_ROUTING_GROUP_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP"
ASIC_METER_POLICY_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_METER_POLICY"
ASIC_METER_RULE_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_METER_RULE"

APP_DB_TO_PROTOBUF_MAP = {
    swsscommon.APP_DASH_APPLIANCE_TABLE_NAME: Appliance,
    swsscommon.APP_DASH_VNET_TABLE_NAME: Vnet,
    swsscommon.APP_DASH_ENI_TABLE_NAME: Eni,
    swsscommon.APP_DASH_VNET_MAPPING_TABLE_NAME: VnetMapping,
    swsscommon.APP_DASH_ROUTE_TABLE_NAME: Route,
    swsscommon.APP_DASH_ROUTE_RULE_TABLE_NAME: RouteRule,
    swsscommon.APP_DASH_ENI_ROUTE_TABLE_NAME: EniRoute,
    swsscommon.APP_DASH_ROUTING_TYPE_TABLE_NAME: RouteType,
    swsscommon.APP_DASH_METER_POLICY_TABLE_NAME: MeterPolicy,
    swsscommon.APP_DASH_METER_RULE_TABLE_NAME: MeterRule,
    swsscommon.APP_DASH_ROUTE_GROUP_TABLE_NAME: RouteGroup,
    swsscommon.APP_DASH_TUNNEL_TABLE_NAME: Tunnel
}


def del_all_keys(dash_db):
    for table_name in APP_DB_TO_PROTOBUF_MAP.keys():
        keys = dash_db.get_app_db_keys(table_name)
        for key in keys:
            dash_db.remove_app_db_entry(table_name, key)


@pytest.fixture
def dash_db(dvs):
    dash_db = DashDB(dvs)
    yield dash_db
    del_all_keys(dash_db)


@pytest.fixture(scope="module")
def dash_db_module(dvs):
    dash_db = DashDB(dvs)
    yield dash_db
    del_all_keys(dash_db)


def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    elif isinstance(value, bytes):
        return value
    return str(value)

class ProducerStateTable(swsscommon.ProducerStateTable):
    def __setitem__(self, key: str, pairs: typing.Union[dict, list, tuple]):
        pairs_str = []
        if isinstance(pairs, dict):
            pairs = pairs.items()
        for k, v in pairs:
            pairs_str.append((to_string(k), to_string(v)))
        self.set(key, pairs_str)
        time.sleep(1)

    def __delitem__(self, key: str):
        self.delete(str(key))
        time.sleep(1)


class Table(swsscommon.Table):
    def __getitem__(self, key: str):
        exists, result = self.get(str(key))
        if not exists:
            return None
        else:
            return dict(result)

    def __contains__(self, key: str):
        return self[key] is not None

    def get_keys(self):
        return self.getKeys()

    def get_newly_created_oid(self, old_oids):
        new_oids = self.asic_db.wait_for_n_keys(self, len(old_oids) + 1)
        oid = [ids for ids in new_oids if ids not in old_oids]
        return oid[0]


class DashDB(object):

    def parse_key_value(self, arglist):
        if len(arglist) < 2:
            raise ValueError("Invalid number of arguments")
        # elif len(arglist) == 1:
            # handle case where no value is passed (e.g. in remove_app_db_entry)
            # key = arglist[0]
            # value = None
        else:
            # concat all parts of the key, assume last arg to be the value
            key = ":".join(arglist[:-1])
            value = arglist[-1]
        return key, value

    def set_app_db_entry(self, table_name, *args):
        key, value = self.parse_key_value(args)
        if isinstance(value, dict):
            pb = ParseDict(value, APP_DB_TO_PROTOBUF_MAP[table_name]())
            pb_string = pb.SerializeToString()
        elif isinstance(value, Message):
            pb_string = value.SerializeToString()
        else:
            pb_string = value

        table = ProducerStateTable(self.dvs.get_dpu_app_db().db_connection, table_name)
        table[key] = {'pb': pb_string}

    def remove_app_db_entry(self, table_name, *key_parts):
        key = ":".join(key_parts)
        table = ProducerStateTable(self.dvs.get_dpu_app_db().db_connection, table_name)
        del table[key]

    def get_asic_db_entry(self, table_name, key):
        table = Table(self.dvs.get_asic_db().db_connection, table_name)
        return table[key]

    def wait_for_asic_db_keys(self, table_name, min_keys=1, old_keys=None):

        def polling_function():
            table = Table(self.dvs.get_asic_db().db_connection, table_name)
            keys = table.get_keys()
            if old_keys:
                keys = [key for key in keys if key not in old_keys]
            return len(keys) >= min_keys, keys

        _, keys = wait_for_result(polling_function, failure_message=f"Found fewer than {min_keys} keys in ASIC_DB table {table_name}")
        return keys

    def wait_for_asic_db_key_del(self, table_name, key):
        def polling_function():
            table = Table(self.dvs.get_asic_db().db_connection, table_name)
            return key not in table, None

        _, attrs = wait_for_result(polling_function, failure_message=f"ASIC_DB table {table_name} still has key {key}")
        return attrs

    def wait_for_asic_db_field(self, table_name, key, field, expected_value=None):

        def polling_function():
            table = Table(self.dvs.get_asic_db().db_connection, table_name)
            attrs = table[key]
            if attrs is None or field not in attrs:
                return False, None

            if expected_value is not None:
                return attrs[field] == expected_value, attrs[field]
            else:
                return True, attrs[field]
        
        if expected_value is not None:
            failure_message = f"Field {field} in ASIC_DB table {table_name} not equal to {expected_value}"
        else:
            failure_message = f"Field {field} not found in ASIC_DB table {table_name}"
        success, value = wait_for_result(polling_function, failure_message=failure_message)
        if success:
            return value
        else:
            return None

    def get_attr_to_sai_object_map(self, table_name, attribute):
        table = Table(self.dvs.get_asic_db().db_connection, table_name)
        keys = table.get_keys()
        attr_to_sai_object_map = {}
        for key in keys:
            attrs = table[key]
            if attribute in attrs:
                attr_to_sai_object_map[attrs[attribute]] = key
        return attr_to_sai_object_map

    def get_app_db_keys(self, table_name):
        table = Table(self.dvs.get_dpu_app_db().db_connection, table_name)
        return table.get_keys()

    def get_asic_db_keys(self, table_name):
        table = Table(self.dvs.get_asic_db().db_connection, table_name)
        return table.get_keys()

    def __init__(self, dvs):
        self.dvs = dvs
        self.app_dash_routing_type_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_ROUTING_TYPE_TABLE")
        self.app_dash_appliance_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_APPLIANCE_TABLE")
        self.app_dash_vnet_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_VNET_TABLE")
        self.app_dash_eni_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_ENI_TABLE")
        self.app_dash_vnet_map_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_VNET_MAPPING_TABLE")
        self.app_dash_route_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_ROUTE_TABLE")
        self.app_dash_route_rule_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_ROUTE_RULE_TABLE")
        self.app_dash_eni_route_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_ENI_ROUTE_TABLE")
        self.app_dash_route_group_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_ROUTE_GROUP_TABLE")
        self.app_dash_meter_policy_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_METER_POLICY_TABLE")
        self.app_dash_meter_rule_table = ProducerStateTable(
            self.dvs.get_dpu_app_db().db_connection, "DASH_METER_RULE_TABLE")

        self.asic_dash_appliance_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_DASH_APPLIANCE")
        self.asic_direction_lookup_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_DIRECTION_LOOKUP_ENTRY")
        self.asic_vip_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_VIP_ENTRY")
        self.asic_dash_vnet_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_VNET")
        self.asic_eni_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ENI")
        self.asic_eni_ether_addr_map_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ENI_ETHER_ADDRESS_MAP_ENTRY")
        self.asic_dash_outbound_ca_to_pa_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_CA_TO_PA_ENTRY")
        self.asic_pa_validation_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_PA_VALIDATION_ENTRY")
        self.asic_outbound_routing_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY")
        self.asic_inbound_routing_rule_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_INBOUND_ROUTING_ENTRY")
        self.asic_outbound_routing_group_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP")
        self.asic_meter_policy_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_METER_POLICY")
        self.asic_meter_rule_table = Table(
            self.dvs.get_asic_db().db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_METER_RULE")

    def create_appliance(self, appliance_id, attr_maps: dict):
        self.app_dash_appliance_table[str(appliance_id)] = attr_maps

    def remove_appliance(self, appliance_id):
        del self.app_dash_appliance_table[str(appliance_id)]

    def create_vnet(self, vnet, attr_maps: dict):
        self.app_dash_vnet_table[str(vnet)] = attr_maps

    def remove_vnet(self, vnet):
        del self.app_dash_vnet_table[str(vnet)]

    def create_eni(self, eni, attr_maps: dict):
        self.app_dash_eni_table[str(eni)] = attr_maps

    def remove_eni(self, eni):
        del self.app_dash_eni_table[str(eni)]

    def create_eni_route(self, eni, attr_maps: dict):
        self.app_dash_eni_route_table[str(eni)] = attr_maps
    
    def remove_eni_route(self, eni):
        del self.app_dash_eni_route_table[str(eni)]

    def create_vnet_mapping(self, vnet, ip, attr_maps: dict):
        self.app_dash_vnet_map_table[str(vnet) + ":" + str(ip)] = attr_maps

    def remove_vnet_mapping(self, vnet, ip):
        del self.app_dash_vnet_map_table[str(vnet) + ":" + str(ip)]

    def create_route(self, route_group, ip, attr_maps: dict):
        self.app_dash_route_table[str(route_group) + ":" + str(ip)] = attr_maps

    def remove_route(self, route_group, ip):
        del self.app_dash_route_table[str(route_group) + ":" + str(ip)]

    def create_route_group(self, route_group, attr_maps: dict):
        self.app_dash_route_group_table[str(route_group)] = attr_maps

    def remove_route_group(self, route_group):
        del self.app_dash_route_group_table[str(route_group)]

    def create_inbound_routing(self, mac_string, vni, ip, attr_maps: dict):
        self.app_dash_route_rule_table[str(mac_string) + ":" + str(vni) + ":" + str(ip)] = attr_maps

    def remove_inbound_routing(self, mac_string, vni, ip):
        del self.app_dash_route_rule_table[str(mac_string) + ":" + str(vni) + ":" + str(ip)]

    def create_routing_type(self, routing_type, attr_maps: dict):
        self.app_dash_routing_type_table[str(routing_type)] = attr_maps

    def remove_routing_type(self, routing_type):
        del self.app_dash_routing_type_table[str(routing_type)]
