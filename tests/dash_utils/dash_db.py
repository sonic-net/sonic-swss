from swsscommon import swsscommon
import typing
import pytest

@pytest.fixture(scope='module')
def dash_db(dvs):
    return DashDB(dvs)

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

    def __delitem__(self, key: str):
        self.delete(str(key))


class Table(swsscommon.Table):
    def __getitem__(self, key: str):
        exists, result = self.get(str(key))
        if not exists:
            return None
        else:
            return dict(result)

    def get_keys(self):
        return self.getKeys()

    def get_newly_created_oid(self, old_oids):
        new_oids = self.asic_db.wait_for_n_keys(self, len(old_oids) + 1)
        oid = [ids for ids in new_oids if ids not in old_oids]
        return oid[0]


class DashDB(object):
    def __init__(self, dvs):
        self.dvs = dvs
        self.app_dash_routing_type_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_ROUTING_TYPE_TABLE")
        self.app_dash_appliance_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_APPLIANCE_TABLE")
        self.app_dash_vnet_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_VNET_TABLE")
        self.app_dash_eni_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_ENI_TABLE")
        self.app_dash_vnet_map_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_VNET_MAPPING_TABLE")
        self.app_dash_route_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_ROUTE_TABLE")
        self.app_dash_route_rule_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_ROUTE_RULE_TABLE")
        self.app_dash_eni_route_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_ENI_ROUTE_TABLE")
        self.app_dash_route_group_table = ProducerStateTable(
            self.dvs.get_app_db().db_connection, "DASH_ROUTE_GROUP_TABLE")

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
