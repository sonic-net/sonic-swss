from swsscommon import swsscommon

import util
import json


class P4RtRouterInterfaceWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT router interface object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = swsscommon.APP_P4RT_ROUTER_INTERFACE_TABLE_NAME
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    SAI_ATTR_SRC_MAC = "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS"
    SAI_ATTR_TYPE = "SAI_ROUTER_INTERFACE_ATTR_TYPE"
    SAI_ATTR_TYPE_PORT = "SAI_ROUTER_INTERFACE_TYPE_PORT"
    SAI_ATTR_MTU = "SAI_ROUTER_INTERFACE_ATTR_MTU"
    SAI_ATTR_PORT_ID = "SAI_ROUTER_INTERFACE_ATTR_PORT_ID"
    SAI_ATTR_DEFAULT_MTU = "9100"

    # attribute fields for router interface object
    PORT_FIELD = "port"
    SRC_MAC_FIELD = "src_mac"

    # default router interface attribute values
    DEFAULT_ROUTER_INTERFACE_ID = "16"
    DEFAULT_PORT_ID = "Ethernet8"
    DEFAULT_SRC_MAC = "00:11:22:33:44:55"
    DEFAULT_ACTION = "set_port_and_src_mac"

    # Fetch oid of the first newly created rif from created rif in ASIC
    # db. This API should only be used when only one oid is expected to be
    # created after the original entries.
    # Original rif entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching oid of newly created rif.
    def get_newly_created_router_interface_oid(self, known_oids=set()):
        rif_oid = None
        rif_entries = util.get_keys(self.asic_db, self.ASIC_DB_TBL_NAME)
        for key in rif_entries:
            if (
                key
                not in self._original_entries[
                    "{}:{}".format(self.asic_db, self.ASIC_DB_TBL_NAME)
                ]
                and
                key not in known_oids
            ):
                rif_oid = key
                break
        return rif_oid

    def generate_app_db_key(self, router_interface_id):
        d = {}
        d[util.prepend_match_field("router_interface_id")] = router_interface_id
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    # create default router interface
    def create_router_interface(
        self, router_interace_id=None, port_id=None, src_mac=None, action=None
    ):
        router_interface_id = router_interace_id or self.DEFAULT_ROUTER_INTERFACE_ID
        port_id = port_id or self.DEFAULT_PORT_ID
        src_mac = src_mac or self.DEFAULT_SRC_MAC
        action = action or self.DEFAULT_ACTION
        attr_list = [
            (util.prepend_param_field(self.PORT_FIELD), port_id),
            (util.prepend_param_field(self.SRC_MAC_FIELD), src_mac),
            (self.ACTION_FIELD, action),
        ]
        router_intf_key = self.generate_app_db_key(router_interface_id)
        self.set_app_db_entry(router_intf_key, attr_list)
        return router_interface_id, router_intf_key, attr_list

class P4RtGreTunnelWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT GRE Tunnel object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = "FIXED_TUNNEL_TABLE"
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    SAI_ATTR_TYPE = "SAI_TUNNEL_ATTR_TYPE"
    SAI_ATTR_PEER_MODE = "SAI_TUNNEL_ATTR_PEER_MODE"
    SAI_ATTR_UNDERLAY_INTERFACE = "SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE"
    SAI_ATTR_OVERLAY_INTERFACE = "SAI_TUNNEL_ATTR_OVERLAY_INTERFACE"
    SAI_ATTR_ENCAP_SRC_IP = "SAI_TUNNEL_ATTR_ENCAP_SRC_IP"
    SAI_ATTR_ENCAP_DST_IP = "SAI_TUNNEL_ATTR_ENCAP_DST_IP"

    # attribute fields for tunnel object
    ROUTER_ROUTER_INTERFACE_ID_FIELD = "router_interface_id"
    ENCAP_SRC_IP_FIELD = "encap_src_ip"
    ENCAP_DST_IP_FIELD = "encap_dst_ip"

    # default tunnel attribute values
    DEFAULT_TUNNEL_ID = "tunnel-1"
    DEFAULT_ROUTER_INTERFACE_ID = "16"
    DEFAULT_ENCAP_SRC_IP = "1.2.3.4"
    DEFAULT_ENCAP_DST_IP = "12.0.0.1"
    DEFAULT_ACTION = "mark_for_p2p_tunnel_encap"

    def generate_app_db_key(self, tunnel_id):
        d = {}
        d[util.prepend_match_field("tunnel_id")] = tunnel_id
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    # create default tunnel
    def create_gre_tunnel(
        self, tunnel_id=None, router_interface_id=None, encap_src_ip=None, encap_dst_ip=None, action=None
    ):
        tunnel_id = tunnel_id or self.DEFAULT_TUNNEL_ID
        router_interface_id = router_interface_id or self.DEFAULT_ROUTER_INTERFACE_ID
        encap_src_ip = encap_src_ip or self.DEFAULT_ENCAP_SRC_IP
        encap_dst_ip = encap_dst_ip or self.DEFAULT_ENCAP_DST_IP
        action = action or self.DEFAULT_ACTION
        attr_list = [
            (util.prepend_param_field(self.ROUTER_ROUTER_INTERFACE_ID_FIELD), router_interface_id),
            (util.prepend_param_field(self.ENCAP_SRC_IP_FIELD), encap_src_ip),
            (util.prepend_param_field(self.ENCAP_DST_IP_FIELD), encap_dst_ip),
            (self.ACTION_FIELD, action),
        ]
        tunnel_key = self.generate_app_db_key(tunnel_id)
        self.set_app_db_entry(tunnel_key, attr_list)
        return tunnel_id, tunnel_key, attr_list

    # Fetch oid of the first newly created tunnel from created tunnels in ASIC
    # db. This API should only be used when only one oid is expected to be
    # created after the original entries.
    # Original tunnel entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching oid of newly created tunnel.
    def get_newly_created_tunnel_oid(self):
        tunnel_oid = None
        tunnel_entries = util.get_keys(self.asic_db, self.ASIC_DB_TBL_NAME)
        for key in tunnel_entries:
            if (
                key
                not in self._original_entries[
                    "{}:{}".format(self.asic_db, self.ASIC_DB_TBL_NAME)
                ]
            ):
                tunnel_oid = key
                break
        return tunnel_oid

    def get_original_appl_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.appl_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_appl_state_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s"
                % (self.appl_state_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_asic_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.asic_db, self.ASIC_DB_TBL_NAME)
            ]
        )

class P4RtNeighborWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT neighbor object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = swsscommon.APP_P4RT_NEIGHBOR_TABLE_NAME
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"
    SAI_ATTR_DST_MAC = "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"

    # attribute fields for neighbor object
    DST_MAC_FIELD = "dst_mac"

    # default neighbor attribute values
    DEFAULT_ROUTER_INTERFACE_ID = "16"
    DEFAULT_IPV4_NEIGHBOR_ID = "12.0.0.1"
    DEFAULT_IPV6_NEIGHBOR_ID = "fe80::21a:11ff:fe17:5f80"
    DEFAULT_DST_MAC = "00:02:03:04:05:06"
    DEFAULT_ACTION = "set_dst_mac"

    def generate_app_db_key(self, router_interface_id, neighbor_id):
        d = {}
        d[util.prepend_match_field("router_interface_id")] = router_interface_id
        d[util.prepend_match_field("neighbor_id")] = neighbor_id
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    # create default neighbor
    def create_neighbor(
        self,
        router_interface_id=None,
        neighbor_id=None,
        dst_mac=None,
        action=None,
        ipv4=True,
    ):
        router_interface_id = router_interface_id or self.DEFAULT_ROUTER_INTERFACE_ID
        neighbor_id = neighbor_id or (
            self.DEFAULT_IPV4_NEIGHBOR_ID if ipv4 else self.DEFAULT_IPV6_NEIGHBOR_ID
        )
        dst_mac = dst_mac or self.DEFAULT_DST_MAC
        action = action or self.DEFAULT_ACTION
        attr_list = [
            (util.prepend_param_field(self.DST_MAC_FIELD), dst_mac),
            (self.ACTION_FIELD, action),
        ]
        neighbor_key = self.generate_app_db_key(router_interface_id, neighbor_id)
        self.set_app_db_entry(neighbor_key, attr_list)
        return neighbor_id, neighbor_key, attr_list


class P4RtNextHopWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT nexthop object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = swsscommon.APP_P4RT_NEXTHOP_TABLE_NAME
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    SAI_ATTR_TYPE = "SAI_NEXT_HOP_ATTR_TYPE"
    SAI_ATTR_IP = "SAI_NEXT_HOP_ATTR_IP"
    SAI_ATTR_TUNNEL_ENCAP = "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP"
    SAI_ATTR_ROUTER_INTF_OID = "SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID"
    SAI_ATTR_TUNNEL_OID = "SAI_NEXT_HOP_ATTR_TUNNEL_ID"

    # attribute fields for nexthop object
    RIF_FIELD = "router_interface_id"
    NEIGHBOR_ID_FIELD = "neighbor_id"
    TUNNEL_ID_FIELD = "tunnel_id"

    # default next hop attribute values
    DEFAULT_ACTION = "set_ip_nexthop"
    DEFAULT_NEXTHOP_ID = "8"
    DEFAULT_ROUTER_INTERFACE_ID = "16"
    DEFAULT_IPV4_NEIGHBOR_ID = "12.0.0.1"
    DEFAULT_IPV6_NEIGHBOR_ID = "fe80::21a:11ff:fe17:5f80"

    # tunnel nexthop attribute values
    TUNNEL_ACTION = "set_p2p_tunnel_encap_nexthop"
    DEFAULT_TUNNEL_ID = "tunnel-1"

    def generate_app_db_key(self, nexthop_id):
        d = {}
        d[util.prepend_match_field("nexthop_id")] = nexthop_id
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    # create next hop
    def create_next_hop(
        self,
        router_interface_id=None,
        neighbor_id=None,
        action=None,
        nexthop_id=None,
        ipv4=True,
        tunnel_id=None,
    ):
        action = action or (self.DEFAULT_ACTION if tunnel_id == None else self.TUNNEL_ACTION)
        router_interface_id = router_interface_id or self.DEFAULT_ROUTER_INTERFACE_ID
        if ipv4 is True:
            neighbor_id = neighbor_id or self.DEFAULT_IPV4_NEIGHBOR_ID
        else:
            neighbor_id = neighbor_id or self.DEFAULT_IPV6_NEIGHBOR_ID
        nexthop_id = nexthop_id or self.DEFAULT_NEXTHOP_ID
        attr_list = [(self.ACTION_FIELD, action)]
        if action == self.DEFAULT_ACTION:
            attr_list.append((util.prepend_param_field(self.RIF_FIELD), router_interface_id))
            attr_list.append((util.prepend_param_field(self.NEIGHBOR_ID_FIELD), neighbor_id))
        if tunnel_id != None:
            attr_list.append((util.prepend_param_field(self.TUNNEL_ID_FIELD), tunnel_id))
        nexthop_key = self.generate_app_db_key(nexthop_id)
        self.set_app_db_entry(nexthop_key, attr_list)
        return nexthop_id, nexthop_key, attr_list

    # Fetch oid of the first newly created nexthop from created nexthops in ASIC
    # db. This API should only be used when only one oid is expected to be
    # created after the original entries.
    # Original nexthop entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching oid of newly created nexthop.
    def get_newly_created_nexthop_oid(self):
        nexthop_oid = None
        nexthop_entries = util.get_keys(self.asic_db, self.ASIC_DB_TBL_NAME)
        for key in nexthop_entries:
            if (
                key
                not in self._original_entries[
                    "{}:{}".format(self.asic_db, self.ASIC_DB_TBL_NAME)
                ]
            ):
                nexthop_oid = key
                break
        return nexthop_oid

    def get_original_appl_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.appl_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_appl_state_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s"
                % (self.appl_state_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_asic_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.asic_db, self.ASIC_DB_TBL_NAME)
            ]
        )

class P4RtWcmpGroupWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT wcmp group object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    TBL_NAME = swsscommon.APP_P4RT_WCMP_GROUP_TABLE_NAME
    ASIC_DB_GROUP_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
    SAI_ATTR_GROUP_TYPE = "SAI_NEXT_HOP_GROUP_ATTR_TYPE"
    SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP = (
        "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP"
    )
    ASIC_DB_GROUP_MEMBER_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"
    SAI_ATTR_GROUP_MEMBER_NEXTHOP_GROUP_ID = (
        "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"
    )
    SAI_ATTR_GROUP_MEMBER_NEXTHOP_ID = "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"
    SAI_ATTR_GROUP_MEMBER_WEIGHT = "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT"

    # attribute fields for wcmp group object
    NEXTHOP_ID_FIELD = "nexthop_id"
    WEIGHT_FIELD = "weight"
    WATCH_PORT_FIELD = "watch_port"
    ACTION_FIELD = "action"
    ACTIONS_FIELD = "actions"

    # default wcmp group attributes
    DEFAULT_WCMP_GROUP_ID = "group-a"
    DEFAULT_WEIGHT = 2
    DEFAULT_ACTION = "set_nexthop_id"
    DEFAULT_NEXTHOP_ID = "8"
    DEFAULT_WATCH_PORT = ""

    # Fetch the oid of the first newly created wcmp group from created wcmp groups
    # in AISC db. This API should only be used when only one oid is expected to be
    # created after the original entries.
    # Original wcmp group entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching oid of newly created wcmp group.
    def get_newly_created_wcmp_group_oid(self):
        wcmp_group_oid = None
        wcmp_group_entries = util.get_keys(self.asic_db, self.ASIC_DB_GROUP_TBL_NAME)
        for key in wcmp_group_entries:
            if (
                key
                not in self._original_entries[
                    "{}:{}".format(self.asic_db, self.ASIC_DB_GROUP_TBL_NAME)
                ]
            ):
                wcmp_group_oid = key
                break
        return wcmp_group_oid

    # Fetch key for the first newly created wcmp group member from created group
    # members in ASIC db. This API should only be used when only one key is
    # expected to be created after the original entries.
    # Original wcmp group member entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching asic db key of newly created
    # wcmp group member.
    def get_newly_created_wcmp_group_member_asic_db_key(self):
        asic_db_wcmp_group_member_key = None
        wcmp_group_member_entries = util.get_keys(
            self.asic_db, self.ASIC_DB_GROUP_MEMBER_TBL_NAME
        )
        for key in wcmp_group_member_entries:
            if (
                key
                not in self._original_entries[
                    "{}:{}".format(self.asic_db, self.ASIC_DB_GROUP_MEMBER_TBL_NAME)
                ]
            ):
                asic_db_wcmp_group_member_key = key
                break
        return asic_db_wcmp_group_member_key

    def generate_app_db_key(self, group_id):
        d = {}
        d[util.prepend_match_field("wcmp_group_id")] = group_id
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    # create default wcmp group
    def create_wcmp_group(
        self,
        nexthop_id=None,
        wcmp_group_id=None,
        action=None,
        weight=None,
        watch_port=None,
    ):
        wcmp_group_id = wcmp_group_id or self.DEFAULT_WCMP_GROUP_ID
        weight = weight or self.DEFAULT_WEIGHT
        action = action or self.DEFAULT_ACTION
        nexthop_id = nexthop_id or self.DEFAULT_NEXTHOP_ID
        watch_port = watch_port or self.DEFAULT_WATCH_PORT
        action1 = {
            util.prepend_param_field(self.NEXTHOP_ID_FIELD): nexthop_id,
            self.WEIGHT_FIELD: weight,
            self.ACTION_FIELD: action,
            self.WATCH_PORT_FIELD: watch_port,
        }
        actions = [action1]
        attr_list = [(self.ACTIONS_FIELD, json.dumps(actions))]
        wcmp_group_key = self.generate_app_db_key(wcmp_group_id)
        self.set_app_db_entry(wcmp_group_key, attr_list)
        return wcmp_group_id, wcmp_group_key, attr_list

    def get_original_appl_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.appl_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_appl_state_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s"
                % (self.appl_state_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_asic_db_group_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.asic_db, self.ASIC_DB_GROUP_TBL_NAME)
            ]
        )

    def get_original_asic_db_member_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.asic_db, self.ASIC_DB_GROUP_MEMBER_TBL_NAME)
            ]
        )


class P4RtRouteWrapper(util.DBInterface):
    """Interface to interact with APP DB and ASIC DB tables for P4RT route object."""

    # database and SAI constants
    APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
    ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    SAI_ATTR_PACKET_ACTION = "SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION"
    SAI_ATTR_PACKET_ACTION_FORWARD = "SAI_PACKET_ACTION_FORWARD"
    SAI_ATTR_PACKET_ACTION_DROP = "SAI_PACKET_ACTION_DROP"
    SAI_ATTR_PACKET_ACTION_TRAP = "SAI_PACKET_ACTION_TRAP"
    SAI_ATTR_NEXTHOP_ID = "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"
    SAI_ATTR_META_DATA = "SAI_ROUTE_ENTRY_ATTR_META_DATA"

    # attribute fields for route object
    NEXTHOP_ID_FIELD = "nexthop_id"
    WCMP_GROUP_ID_FIELD = "wcmp_group_id"
    ROUTE_METADATA_FIELD = "route_metadata"

    # default route attribute values
    DEFAULT_ACTION = "set_nexthop_id"
    DEFAULT_NEXTHOP_ID = "8"
    DEFAULT_WCMP_GROUP_ID = "group-a"
    DEFAULT_VRF_ID = "b4-traffic"
    DEFAULT_DST = "10.11.12.0/24"

    def generate_app_db_key(self, vrf_id, dst):
        assert self.ip_type is not None
        d = {}
        d[util.prepend_match_field("vrf_id")] = vrf_id
        if self.ip_type == "IPV4":
            d[util.prepend_match_field("ipv4_dst")] = dst
        else:
            d[util.prepend_match_field("ipv6_dst")] = dst
        key = json.dumps(d, separators=(",", ":"))
        return self.TBL_NAME + ":" + key

    def set_ip_type(self, ip_type):
        assert ip_type in ("IPV4", "IPV6")
        self.ip_type = ip_type
        self.TBL_NAME = "FIXED_" + ip_type + "_TABLE"

    # Create default route.
    def create_route(
        self,
        nexthop_id=None,
        wcmp_group_id=None,
        action=None,
        vrf_id=None,
        dst=None,
        metadata="",
    ):
        action = action or self.DEFAULT_ACTION
        vrf_id = vrf_id or self.DEFAULT_VRF_ID
        dst = dst or self.DEFAULT_DST
        if action == "set_wcmp_group_id":
            wcmp_group_id = wcmp_group_id or self.DEFAULT_WCMP_GROUP_ID
            attr_list = [
                (self.ACTION_FIELD, action),
                (util.prepend_param_field(self.WCMP_GROUP_ID_FIELD), wcmp_group_id),
            ]
        elif action == "set_nexthop_id":
            nexthop_id = nexthop_id or self.DEFAULT_NEXTHOP_ID
            attr_list = [
                (self.ACTION_FIELD, action),
                (util.prepend_param_field(self.NEXTHOP_ID_FIELD), nexthop_id),
            ]
        elif action == "set_wcmp_group_id_and_metadata":
            wcmp_group_id = wcmp_group_id or self.DEFAULT_WCMP_GROUP_ID
            attr_list = [
                (self.ACTION_FIELD, action),
                (util.prepend_param_field(self.WCMP_GROUP_ID_FIELD), wcmp_group_id),
                (util.prepend_param_field(self.ROUTE_METADATA_FIELD), metadata),
            ]
        elif action == "set_nexthop_id_and_metadata":
            nexthop_id = nexthop_id or self.DEFAULT_NEXTHOP_ID
            attr_list = [
                (self.ACTION_FIELD, action),
                (util.prepend_param_field(self.NEXTHOP_ID_FIELD), nexthop_id),
                (util.prepend_param_field(self.ROUTE_METADATA_FIELD), metadata),
            ]
        else:
            attr_list = [(self.ACTION_FIELD, action)]
        route_key = self.generate_app_db_key(vrf_id, dst)
        self.set_app_db_entry(route_key, attr_list)
        return route_key, attr_list

    # Fetch the asic_db_key for the first newly created route entry from created
    # routes in ASIC db. This API should only be used when only one key is
    # expected to be created after original entries.
    # Original route entries in asic db must be fetched using
    # 'get_original_redis_entries' before fetching asic db key of newly created
    # route.
    def get_newly_created_asic_db_key(self):
        route_entries = util.get_keys(self.asic_db, self.ASIC_DB_TBL_NAME)
        for key in route_entries:
            if (
                key
                not in self._original_entries[
                    "%s:%s" % (self.asic_db, self.ASIC_DB_TBL_NAME)
                ]
            ):
                asic_db_key = key
                break
        return asic_db_key

    def get_original_appl_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s" % (self.appl_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_appl_state_db_entries_count(self):
        return len(
            self._original_entries[
                "%s:%s"
                % (self.appl_state_db, (self.APP_DB_TBL_NAME + ":" + self.TBL_NAME))
            ]
        )

    def get_original_asic_db_entries_count(self):
        return len(
            self._original_entries["%s:%s" % (self.asic_db, self.ASIC_DB_TBL_NAME)]
        )
