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
  SAI_ATTR_MTU = "SAI_ROUTER_INTERFACE_ATTR_MTU"
  SAI_ATTR_PORT_ID = "SAI_ROUTER_INTERFACE_ATTR_PORT_ID"
  SAI_ATTR_V4_MCAST_ENABLE = "SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE"
  SAI_ATTR_V6_MCAST_ENABLE = "SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE"
  SAI_ATTR_DEFAULT_MTU = "9100"

  # Attribute fields for multicast router interface entry.
  ACTION_FIELD = "action"
  SRC_MAC_FIELD = "src_mac"

  # Default router interface attribute values.
  DEFAULT_PORT_ID = "Ethernet8"
  DEFAULT_INSTANCE = "0"
  DEFAULT_SRC_MAC = "00:11:22:33:44:55"
  DEFAULT_ACTION = "set_src_mac"

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


class P4RtL3MulticastReplicationWrapper(util.DBInterface):
  """Interface to interact with APP DB and ASIC DB tables for P4RT L3 multicast replication object."""

  # Database and SAI constants.
  APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
  TBL_NAME = swsscommon.APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME

  ASIC_DB_GROUP_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_IPMC_GROUP"
  ASIC_DB_GROUP_MEMBER_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER"
  SAI_ATTR_IPMC_GROUP_ID = "SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_GROUP_ID"
  SAI_ATTR_IPMC_OUTPUT_ID = "SAI_IPMC_GROUP_MEMBER_ATTR_IPMC_OUTPUT_ID"

  # Default router interface attribute values.
  DEFAULT_GROUP_ID = "0"
  DEFAULT_PORT_ID = "Ethernet8"
  DEFAULT_INSTANCE = "0"

  CONTROLLER_METADATA = "controller_metadata"
  DEFAULT_METADATA = "my_metadata"

  def generate_app_db_key(self, multicast_group_id, multicast_replica_port,
                          multicast_replica_instance):
    d = {}
    d[util.prepend_match_field("multicast_group_id")] = (
        multicast_group_id)
    d[util.prepend_match_field("multicast_replica_port")] = (
        multicast_replica_port)
    d[util.prepend_match_field("multicast_replica_instance")] = (
        multicast_replica_instance)
    key = json.dumps(d, separators=(",", ":"))
    return self.TBL_NAME + ":" + key

  # Create default multicast group member.
  def create_multicast_group_member(self, group_id=None, port_id=None,
                                    instance=None):
    group_id = group_id or self.DEFAULT_GROUP_ID
    port_id = port_id or self.DEFAULT_PORT_ID
    instance = instance or self.DEFAULT_INSTANCE
    # Hmm, we need attributes, otherwise this is treated as a delete?
    attr_list = [
        (self.CONTROLLER_METADATA, self.DEFAULT_METADATA),
    ]
    mcast_replication_key = self.generate_app_db_key(group_id, port_id,
                                                     instance)
    self.set_app_db_entry(mcast_replication_key, attr_list)
    return mcast_replication_key, attr_list


class P4RtL3MulticastRouteWrapper(util.DBInterface):
  """Interface to interact with APP DB and ASIC DB tables for P4RT L3 multicast route object."""

  # Database and SAI constants.
  APP_DB_TBL_NAME = swsscommon.APP_P4RT_TABLE_NAME
  TBL_NAME_IPV4 = swsscommon.APP_P4RT_IPV4_TABLE_NAME
  TBL_NAME_IPV6 = swsscommon.APP_P4RT_IPV6_TABLE_NAME

  ASIC_DB_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_IPMC_ENTRY"
  ASIC_DB_RPF_GROUP_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_RPF_GROUP"
  SAI_ATTR_PACKET_ACTION = "SAI_IPMC_ENTRY_ATTR_PACKET_ACTION"
  SAI_ATTR_PACKET_ACTION_FORWARD = "SAI_PACKET_ACTION_FORWARD"
  SAI_ATTR_OUTPUT_GROUP_ID = "SAI_IPMC_ENTRY_ATTR_OUTPUT_GROUP_ID"
  SAI_ATTR_RPF_GROUP_ID = "SAI_IPMC_ENTRY_ATTR_RPF_GROUP_ID"

  # Attribute fields for multicast route entry.
  ACTION_FIELD = "action"
  MULTICAST_GROUP_ID_FIELD = "multicast_group_id"
  NEXT_HOP_ID_FIELD = "nexthop_id"

  # Default router interface attribute values.
  DEFAULT_ACTION = "set_multicast_group_id"
  SET_NEXT_HOP_ID_ACTION = "set_nexthop_id"
  DEFAULT_GROUP_ID = "0"
  DEFAULT_VRF_ID = "b4-traffic"
  DEFAULT_DST_V6 = "2001:db8:3:4:5:6:7:8"
  DEFAULT_DST_V4 = "10.11.12.0"

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
                             action=None, param="0"):
    group_id = group_id or self.DEFAULT_GROUP_ID
    if is_v4:
      ipv4_dst = dst_ip or self.DEFAULT_DST_V4
      ipv6_dst = None
    else:
      ipv4_dst = None
      ipv6_dst = dst_ip or self.DEFAULT_DST_V6
    vrf_id = self.DEFAULT_VRF_ID
    action = action or self.DEFAULT_ACTION
    if action == self.DEFAULT_ACTION:
      attr_list = [
          (util.prepend_param_field(self.MULTICAST_GROUP_ID_FIELD), group_id),
          (self.ACTION_FIELD, action),
      ]
    else:
      attr_list = [
          (util.prepend_param_field(self.NEXT_HOP_ID_FIELD), param),
          (self.ACTION_FIELD, action),
      ]
    mcast_route_key = self.generate_app_db_key(vrf_id, ipv4_dst, ipv6_dst)
    self.set_app_db_entry(mcast_route_key, attr_list)
    return mcast_route_key, attr_list
