from swsscommon import swsscommon

import util
import json


class P4RtL3MulticastRouterInterfaceWrapper(util.DBInterface):
  """Interface to interact with APP DB and ASIC DB tables for P4RT L3 multicast router interface object."""

  # Database and SAI constants.
  APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
  TBL_NAME = swsscommon.APP_P4RT_MULTICAST_ROUTER_INTERFACE_TABLE_NAME

  ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
  SAI_ATTR_VIRTUAL_ROUTER_ID = "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID"
  SAI_ATTR_SRC_MAC = "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS"
  SAI_ATTR_TYPE = "SAI_ROUTER_INTERFACE_ATTR_TYPE"
  SAI_ATTR_TYPE_PORT = "SAI_ROUTER_INTERFACE_TYPE_PORT"
  SAI_ATTR_TYPE_SUB_PORT = "SAI_ROUTER_INTERFACE_TYPE_SUB_PORT"
  SAI_ATTR_OUTER_VLAN_ID = "SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID"
  SAI_ATTR_MTU = "SAI_ROUTER_INTERFACE_ATTR_MTU"
  SAI_ATTR_PORT_ID = "SAI_ROUTER_INTERFACE_ATTR_PORT_ID"
  SAI_ATTR_V4_MCAST_ENABLE = "SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE"
  SAI_ATTR_V6_MCAST_ENABLE = "SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE"
  SAI_ATTR_MY_MAC = "SAI_ROUTER_INTERFACE_ATTR_MY_MAC"
  SAI_ATTR_DEFAULT_MTU = "9100"

  L2_ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"
  SAI_BRIDGE_PORT_ATTR_TYPE = "SAI_BRIDGE_PORT_ATTR_TYPE"
  SAI_BRIDGE_PORT_ATTR_PORT_ID = "SAI_BRIDGE_PORT_ATTR_PORT_ID"
  SAI_BRIDGE_PORT_TYPE_PORT = "SAI_BRIDGE_PORT_TYPE_PORT"
  SAI_BRIDGE_PORT_ATTR_ADMIN_STATE = "SAI_BRIDGE_PORT_ATTR_ADMIN_STATE"
  SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE = "SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE"
  SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE = "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE"

  NEXT_HOP_ASIC_DB_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
  SAI_NEXT_HOP_ATTR_TYPE = "SAI_NEXT_HOP_ATTR_TYPE"
  SAI_NEXT_HOP_ATTR_TYPE_IPMC = "SAI_NEXT_HOP_TYPE_IPMC"
  SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID = "SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID"
  SAI_NEXT_HOP_ATTR_IP = "SAI_NEXT_HOP_ATTR_IP"
  SAI_NEXT_HOP_ATTR_DISABLE_SRC_MAC_REWRITE = "SAI_NEXT_HOP_ATTR_DISABLE_SRC_MAC_REWRITE"
  SAI_NEXT_HOP_ATTR_DISABLE_DST_MAC_REWRITE = "SAI_NEXT_HOP_ATTR_DISABLE_DST_MAC_REWRITE"
  SAI_NEXT_HOP_ATTR_DISABLE_VLAN_REWRITE = "SAI_NEXT_HOP_ATTR_DISABLE_VLAN_REWRITE"

  NEIGHBOR_ENTRY_ASIC_DB_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"
  SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS = "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"
  SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE = "SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE"

  MY_MAC_ASIC_DB_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_MY_MAC"

  # Attribute fields for multicast router interface entry.
  ACTION_FIELD = "action"
  SRC_MAC_FIELD = "src_mac"
  DST_MAC_FIELD = "dst_mac"
  VLAN_ID_FIELD = "vlan_id"

  # Default router interface attribute values.
  DEFAULT_PORT_ID = "Ethernet8"
  DEFAULT_INSTANCE = "0x0"
  UNUSED_MAC_ADDRESS = "00:00:00:00:00:01"
  DEFAULT_SRC_MAC = "00:11:22:33:44:55"
  DEFAULT_DST_MAC = "00:AA:BB:CC:DD:EE"
  DEFAULT_VLAN_ID = "0x123"
  # TODO(b/353398275): Make default action multicast_set_src_mac
  DEFAULT_ACTION = "set_multicast_src_mac"
  MULTICAST_SET_SRC_MAC = "multicast_set_src_mac"
  MULTICAST_SET_SRC_MAC_AND_VLAN_ID = "multicast_set_src_mac_and_vlan_id"
  MULTICAST_SET_SRC_MAC_AND_DST_MAC_AND_VLAN_ID = "multicast_set_src_mac_and_dst_mac_and_vlan_id"
  MULTICAST_SET_SRC_MAC_AND_PRESERVE_INGRESS_VLAN_ID = "multicast_set_src_mac_and_preserve_ingress_vlan_id"
  MULTICAST_L2_PASSTHROUGH_ACTION = "multicast_l2_passthrough"

  def generate_app_db_key(self, multicast_replica_port,
                          multicast_replica_instance):
    d = {}
    d[util.prepend_match_field("multicast_replica_port")] = (
        multicast_replica_port)
    d[util.prepend_match_field("multicast_replica_instance")] = (
        multicast_replica_instance)
    key = json.dumps(d, separators=(",", ":"))
    return self.TBL_NAME + ":" + key

  # Create default router interface.
  def create_router_interface(self, port_id=None, instance=None,
                              src_mac=None):
    port_id = port_id or self.DEFAULT_PORT_ID
    instance = instance or self.DEFAULT_INSTANCE
    src_mac = src_mac or self.DEFAULT_SRC_MAC
    action = self.DEFAULT_ACTION
    attr_list = [
        (util.prepend_param_field(self.SRC_MAC_FIELD), src_mac),
        (self.ACTION_FIELD, action),
    ]
    mcast_router_intf_key = self.generate_app_db_key(port_id, instance)
    self.set_app_db_entry(mcast_router_intf_key, attr_list)
    return mcast_router_intf_key, attr_list

  # Create router interface for new multicast actions.
  def create_router_interface_with_next_hop(self, port_id=None, instance=None,
                                            src_mac=None, dst_mac=None,
                                            vlan_id=None, action=None):
    port_id = port_id or self.DEFAULT_PORT_ID
    instance = instance or self.DEFAULT_INSTANCE
    src_mac = src_mac or self.DEFAULT_SRC_MAC
    dst_mac = dst_mac or self.DEFAULT_DST_MAC
    vlan_id = vlan_id or self.DEFAULT_VLAN_ID
    action = action or self.MULTICAST_SET_SRC_MAC
    attr_list = []

    if action == self.MULTICAST_SET_SRC_MAC:
      attr_list = [
        (util.prepend_param_field(self.SRC_MAC_FIELD), src_mac),
        (self.ACTION_FIELD, action),
      ]
    elif action == self.MULTICAST_SET_SRC_MAC_AND_VLAN_ID:
      attr_list = [
        (util.prepend_param_field(self.SRC_MAC_FIELD), src_mac),
        (util.prepend_param_field(self.VLAN_ID_FIELD), vlan_id),
        (self.ACTION_FIELD, action),
      ]
    elif action == self.MULTICAST_SET_SRC_MAC_AND_DST_MAC_AND_VLAN_ID:
      attr_list = [
        (util.prepend_param_field(self.SRC_MAC_FIELD), src_mac),
        (util.prepend_param_field(self.DST_MAC_FIELD), dst_mac),
        (util.prepend_param_field(self.VLAN_ID_FIELD), vlan_id),
        (self.ACTION_FIELD, action),
      ]
    elif action == self.MULTICAST_SET_SRC_MAC_AND_PRESERVE_INGRESS_VLAN_ID:
      attr_list = [
        (util.prepend_param_field(self.SRC_MAC_FIELD), src_mac),
        (self.ACTION_FIELD, action),
      ]
    mcast_router_intf_key = self.generate_app_db_key(port_id, instance)
    self.set_app_db_entry(mcast_router_intf_key, attr_list)
    return mcast_router_intf_key, attr_list

  # Create default bridge port for L2 replication.
  def create_bridge_port(self, port_id=None, instance=None):
    port_id = port_id or self.DEFAULT_PORT_ID
    instance = instance or self.DEFAULT_INSTANCE
    action = self.MULTICAST_L2_PASSTHROUGH_ACTION
    attr_list = [(self.ACTION_FIELD, action)]
    mcast_router_intf_key = self.generate_app_db_key(port_id, instance)
    self.set_app_db_entry(mcast_router_intf_key, attr_list)
    return mcast_router_intf_key, attr_list


class P4RtL3MulticastGroupWrapper(util.DBInterface):
  """Interface to interact with APP DB and ASIC DB tables for P4RT L3 multicast group object."""

  # Database and SAI constants.
  APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
  TBL_NAME = swsscommon.APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME

  ASIC_DB_GROUP_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_IPMC_GROUP"
  ASIC_DB_GROUP_MEMBER_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER"
  SAI_ATTR_IPMC_GROUP_ID = "SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID"
  SAI_ATTR_IPMC_OUTPUT_ID = "SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID"

  L2_ASIC_DB_GROUP_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_L2MC_GROUP"
  L2_ASIC_DB_GROUP_MEMBER_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_L2MC_GROUP_MEMBER"
  SAI_ATTR_L2MC_GROUP_ID = "SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_GROUP_ID"
  SAI_ATTR_L2MC_OUTPUT_ID = "SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_OUTPUT_ID"

  # Default router interface attribute values.
  DEFAULT_GROUP_ID = "0x1"
  DEFAULT_REPLICAS = [("Ethernet8", "0x0")]

  CONTROLLER_METADATA = "controller_metadata"
  DEFAULT_METADATA = "my_metadata"

  def generate_app_db_key(self, multicast_group_id):
    return self.TBL_NAME + ":" + multicast_group_id

  # Create default multicast group entry.
  def create_multicast_group_entry(self, group_id=None, replicas=None):
    group_id = group_id or self.DEFAULT_GROUP_ID
    if replicas is None:
      local_replicas = [x for x in self.DEFAULT_REPLICAS]
    else:
      local_replicas = replicas

    replica_dicts = []
    for port_name, instance in local_replicas:
      replica_dicts.append(
          "{\"multicast_replica_instance\":\"" + instance  + "\"" +
          ",\"multicast_replica_port\":\"" + port_name + "\"}")
    replica_json_array = "[" + ",".join(replica_dicts) + "]"
    attr_list = [("replicas", replica_json_array)]

    mcast_group_key = self.generate_app_db_key(group_id)
    self.set_app_db_entry(mcast_group_key, attr_list)
    return mcast_group_key, attr_list


class P4RtIpMulticastWrapper(util.DBInterface):
  """Interface to interact with APP DB and ASIC DB tables for P4RT IP multicast table object."""

  # Database and SAI constants.
  APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
  TBL_NAME_IPV4 = swsscommon.APP_P4RT_IPV4_MULTICAST_TABLE_NAME
  TBL_NAME_IPV6 = swsscommon.APP_P4RT_IPV6_MULTICAST_TABLE_NAME

  ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_IPMC_ENTRY"
  ASIC_DB_RPF_GROUP_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_RPF_GROUP"
  SAI_ATTR_PACKET_ACTION = "SAI_IPMC_ENTRY_ATTR_PACKET_ACTION"
  SAI_ATTR_PACKET_ACTION_FORWARD = "SAI_PACKET_ACTION_FORWARD"
  SAI_ATTR_OUTPUT_GROUP_ID = "SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID"
  SAI_ATTR_RPF_GROUP_ID = "SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID"

  # Attribute fields for multicast route entry.
  ACTION_FIELD = "action"
  MULTICAST_GROUP_ID_FIELD = "multicast_group_id"

  # Default router interface attribute values.
  DEFAULT_ACTION = "set_multicast_group_id"
  DEFAULT_GROUP_ID = "0x1"
  DEFAULT_VRF_ID = "b4-traffic"
  DEFAULT_DST_V6 = "ff00:db8:3:4:5:6:7:8"
  DEFAULT_DST_V4 = "225.11.12.0"

  def generate_app_db_key(self, vrf_id, ipv4_dst=None, ipv6_dst=None):
    d = {}
    d[util.prepend_match_field("vrf_id")] = vrf_id
    if ipv4_dst is None:
      d[util.prepend_match_field("ipv6_dst")] = ipv6_dst
      tbl_name = self.TBL_NAME_IPV6
    else:
      d[util.prepend_match_field("ipv4_dst")] = ipv4_dst
      tbl_name = self.TBL_NAME_IPV4
    key = json.dumps(d, separators=(",", ":"))
    return tbl_name + ":" + key

  # Create default multicast route entry.
  def create_multicast_route(self, group_id=None, dst_ip=None, is_v4=True,
                             param="0"):
    group_id = group_id or self.DEFAULT_GROUP_ID
    if is_v4:
      ipv4_dst = dst_ip or self.DEFAULT_DST_V4
      ipv6_dst = None
    else:
      ipv4_dst = None
      ipv6_dst = dst_ip or self.DEFAULT_DST_V6
    vrf_id = self.DEFAULT_VRF_ID
    action = self.DEFAULT_ACTION
    attr_list = [
        (util.prepend_param_field(self.MULTICAST_GROUP_ID_FIELD), group_id),
        (self.ACTION_FIELD, action),
    ]
    mcast_route_key = self.generate_app_db_key(vrf_id, ipv4_dst, ipv6_dst)
    self.set_app_db_entry(mcast_route_key, attr_list)
    return mcast_route_key, attr_list
