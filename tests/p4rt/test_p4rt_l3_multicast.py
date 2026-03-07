# Lint as: python3
from swsscommon import swsscommon

import pytest
import util
import l3
import l3_multicast
import test_vrf


class TestP4RTL3MulticastRouterInterface(object):
  """Tests interacting with multicast router interface table"""

  def _set_up(self, dvs):
    self._p4rt_l3_multicast_router_intf = (
        l3_multicast.P4RtL3MulticastRouterInterfaceWrapper())

    self._p4rt_l3_multicast_router_intf.set_up_databases(dvs)

    self.p4rt_notifier = swsscommon.NotificationProducer(
      self._p4rt_l3_multicast_router_intf.appl_db,
      swsscommon.APP_P4RT_CHANNEL_NAME)
    self.response_consumer = swsscommon.NotificationConsumer(
      self._p4rt_l3_multicast_router_intf.appl_db, "APPL_DB_" +
      swsscommon.APP_P4RT_TABLE_NAME + "_RESPONSE_CHANNEL"
    )
    self.appl_db_table = (
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME + ":" +
            self._p4rt_l3_multicast_router_intf.TBL_NAME)
    self.asic_db_table = self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME

  def get_global_vrf_id(self):
    virt_entries = util.get_keys(self._p4rt_l3_multicast_router_intf.asic_db,
                                 "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
    for key in virt_entries:
      return key
    return "0"

  def test_L3MulticastRouterInterfaceAddUpdateDelete(self, dvs, testlog):
    """
    This test attempts to add a multicast router interface entry, confirms the
    databases are setup correctly, updates the entry to use a different MAC
    address, confirms the databases are setup correctly, and then deletes the
    entry.
    """
    # Initialize database connectors
    self._set_up(dvs)

    # Fetch database state after init.
    original_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    original_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)

    ####################################
    # Add operation
    ####################################
    # Add one L3 multicast router interface entry.
    mcast_router_intf_key, attr_list = (
        self._p4rt_l3_multicast_router_intf.create_router_interface(
            port_id=None, instance=None, src_mac=None))
    util.verify_response(self.response_consumer, mcast_router_intf_key,
                         attr_list, "SWSS_RC_SUCCESS")

    # Check that APP DB has expected entry with expected values.
    mcast_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    assert len(mcast_rif_entries) == (len(original_app_db_entries) + 1)

    (status, fvs) = util.get_key(
        self._p4rt_l3_multicast_router_intf.appl_db,
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME,
        mcast_router_intf_key)
    assert status == True
    util.verify_attr(fvs, attr_list)

    # Check that ASIC DB has expected values.
    mcast_rif_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)
    assert len(mcast_rif_asic_entries) == (len(original_asic_db_entries) + 1)

    asic_db_key = None
    for key in mcast_rif_asic_entries:
      if key not in original_asic_db_entries:
        asic_db_key = key
        break
    assert asic_db_key is not None
    (status, fvs) = util.get_key(
        self._p4rt_l3_multicast_router_intf.asic_db,
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME,
        asic_db_key)
    assert status == True

    global_vrf_id = self.get_global_vrf_id()
    port_oid = util.get_port_oid_by_name(
        dvs, self._p4rt_l3_multicast_router_intf.DEFAULT_PORT_ID)

    attr_list = [
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_VIRTUAL_ROUTER_ID,
         global_vrf_id),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_SRC_MAC,
         self._p4rt_l3_multicast_router_intf.DEFAULT_SRC_MAC),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_TYPE,
         self._p4rt_l3_multicast_router_intf.SAI_ATTR_TYPE_PORT),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_MTU,
         self._p4rt_l3_multicast_router_intf.SAI_ATTR_DEFAULT_MTU),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_PORT_ID, port_oid),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_V4_MCAST_ENABLE, "true"),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_V6_MCAST_ENABLE, "true"),
    ]
    util.verify_attr(fvs, attr_list)

    ####################################
    # Update operation
    ####################################
    # Update L3 multicast router interface entry to use a different MAC.
    new_src_mac = "00:66:77:88:99:AA"
    mcast_router_intf_key, attr_list = (
        self._p4rt_l3_multicast_router_intf.create_router_interface(
            port_id=None, instance=None, src_mac=new_src_mac))
    util.verify_response(self.response_consumer, mcast_router_intf_key,
                         attr_list, "SWSS_RC_SUCCESS")

    # Check that APP DB has expected entry with expected values.
    mcast_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    assert len(mcast_rif_entries) == (len(original_app_db_entries) + 1)

    (status, fvs) = util.get_key(
        self._p4rt_l3_multicast_router_intf.appl_db,
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME,
        mcast_router_intf_key)
    assert status == True
    util.verify_attr(fvs, attr_list)

    # Check that ASIC DB has expected values.
    mcast_rif_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)
    assert len(mcast_rif_asic_entries) == (len(original_asic_db_entries) + 1)

    asic_db_key = None
    for key in mcast_rif_asic_entries:
      if key not in original_asic_db_entries:
        asic_db_key = key
        break
    assert asic_db_key is not None
    (status, fvs) = util.get_key(
        self._p4rt_l3_multicast_router_intf.asic_db,
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME,
        asic_db_key)
    assert status == True

    attr_list = [
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_VIRTUAL_ROUTER_ID,
         global_vrf_id),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_SRC_MAC, new_src_mac),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_TYPE,
         self._p4rt_l3_multicast_router_intf.SAI_ATTR_TYPE_PORT),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_MTU,
         self._p4rt_l3_multicast_router_intf.SAI_ATTR_DEFAULT_MTU),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_PORT_ID, port_oid),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_V4_MCAST_ENABLE, "true"),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_V6_MCAST_ENABLE, "true"),
    ]
    util.verify_attr(fvs, attr_list)

    ####################################
    # Delete operation
    ####################################
    self._p4rt_l3_multicast_router_intf.remove_app_db_entry(
        mcast_router_intf_key)
    util.verify_response(self.response_consumer, mcast_router_intf_key, [],
                         "SWSS_RC_SUCCESS")

    # Check that entries are gone.
    mcast_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    assert len(mcast_rif_entries) == len(original_app_db_entries)

    # Check that ASIC DB has expected values.
    mcast_rif_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)
    assert len(mcast_rif_asic_entries) == len(original_asic_db_entries)

  def test_L3MulticastRouterInterfaceDeleteUnknown(self, dvs, testlog):
    """
    This test attempts to delete an unknown multicast router interface entry,
    which should result in an error.
    """
    self._set_up(dvs)

    # Fetch database state after init.
    original_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    original_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)

    ####################################
    # Delete operation
    ####################################
    mcast_router_intf_key = (
        self._p4rt_l3_multicast_router_intf.generate_app_db_key(
            self._p4rt_l3_multicast_router_intf.DEFAULT_PORT_ID,
            self._p4rt_l3_multicast_router_intf.DEFAULT_INSTANCE))

    self._p4rt_l3_multicast_router_intf.remove_app_db_entry(
        mcast_router_intf_key)
    util.verify_response(
        self.response_consumer, mcast_router_intf_key, [], "SWSS_RC_NOT_FOUND",
        "[OrchAgent] Multicast router interface entry exists does not exist")

    # Check that entries remain unchanged.
    mcast_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    assert len(mcast_rif_entries) == len(original_app_db_entries)

    mcast_rif_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)
    assert len(mcast_rif_asic_entries) == len(original_asic_db_entries)

  def test_L3MulticastRouterInterfaceAddTwoDeleteOne(self, dvs, testlog):
    """
    This tests two entries that will end up sharing a RIF.  When we delete
    one of the entries, we want to confirm that the RIF remains.
    """
    self._set_up(dvs)

    # Fetch database state after init.
    original_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    original_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)

    ####################################
    # Add operation
    ####################################
    # Add two L3 multicast router interface entries.
    mcast_router_intf_key_0, attr_list_0 = (
        self._p4rt_l3_multicast_router_intf.create_router_interface(
            port_id=None, instance="0x0", src_mac=None))
    util.verify_response(self.response_consumer, mcast_router_intf_key_0,
                         attr_list_0, "SWSS_RC_SUCCESS")

    mcast_router_intf_key_1, attr_list_1 = (
        self._p4rt_l3_multicast_router_intf.create_router_interface(
            port_id=None, instance="0x1", src_mac=None))
    util.verify_response(self.response_consumer, mcast_router_intf_key_1,
                         attr_list_1, "SWSS_RC_SUCCESS")

    # Check that APP DB has expected entry with expected values.
    mcast_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    assert len(mcast_rif_entries) == (len(original_app_db_entries) + 2)

    (status_0, fvs_0) = util.get_key(
        self._p4rt_l3_multicast_router_intf.appl_db,
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME,
        mcast_router_intf_key_0)
    assert status_0 == True
    util.verify_attr(fvs_0, attr_list_0)

    (status_1, fvs_1) = util.get_key(
        self._p4rt_l3_multicast_router_intf.appl_db,
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME,
        mcast_router_intf_key_1)
    assert status_1 == True
    util.verify_attr(fvs_1, attr_list_1)

    # Check that ASIC DB has expected values.
    # It's only one entry, because we share a RIF.
    mcast_rif_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)
    assert len(mcast_rif_asic_entries) == (len(original_asic_db_entries) + 1)

    asic_db_key = None
    for key in mcast_rif_asic_entries:
      if key not in original_asic_db_entries:
        asic_db_key = key
        break
    assert asic_db_key is not None
    (status_asic, fvs_asic) = util.get_key(
        self._p4rt_l3_multicast_router_intf.asic_db,
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME,
        asic_db_key)
    assert status_asic == True

    global_vrf_id = self.get_global_vrf_id()
    port_oid = util.get_port_oid_by_name(
        dvs, self._p4rt_l3_multicast_router_intf.DEFAULT_PORT_ID)

    asic_attr_list = [
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_VIRTUAL_ROUTER_ID,
         global_vrf_id),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_SRC_MAC,
         self._p4rt_l3_multicast_router_intf.DEFAULT_SRC_MAC),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_TYPE,
         self._p4rt_l3_multicast_router_intf.SAI_ATTR_TYPE_PORT),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_MTU,
         self._p4rt_l3_multicast_router_intf.SAI_ATTR_DEFAULT_MTU),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_PORT_ID, port_oid),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_V4_MCAST_ENABLE, "true"),
        (self._p4rt_l3_multicast_router_intf.SAI_ATTR_V6_MCAST_ENABLE, "true"),
    ]
    util.verify_attr(fvs_asic, asic_attr_list)

    ####################################
    # Delete operation
    ####################################
    self._p4rt_l3_multicast_router_intf.remove_app_db_entry(
        mcast_router_intf_key_0)
    util.verify_response(self.response_consumer, mcast_router_intf_key_0, [],
                         "SWSS_RC_SUCCESS")

    # Check that one APP DB entry has been removed.
    mcast_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.appl_db, self.appl_db_table)
    assert len(mcast_rif_entries) == (len(original_app_db_entries) + 1)

    (status_0, fvs_0) = util.get_key(
        self._p4rt_l3_multicast_router_intf.appl_db,
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME,
        mcast_router_intf_key_0)
    assert status_0 == False  # We removed the entry.

    (status_1, fvs_1) = util.get_key(
        self._p4rt_l3_multicast_router_intf.appl_db,
        self._p4rt_l3_multicast_router_intf.APP_DB_TBL_NAME,
        mcast_router_intf_key_1)
    assert status_1 == True
    util.verify_attr(fvs_1, attr_list_1)

    # Check that ASIC DB has not been changed after adds.
    mcast_rif_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_router_intf.asic_db, self.asic_db_table)
    assert len(mcast_rif_asic_entries) == (len(original_asic_db_entries) + 1)

    asic_db_key = None
    for key in mcast_rif_asic_entries:
      if key not in original_asic_db_entries:
        asic_db_key = key
        break
    assert asic_db_key is not None
    (status_asic, fvs_asic) = util.get_key(
        self._p4rt_l3_multicast_router_intf.asic_db,
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME,
        asic_db_key)
    assert status_asic == True
    # asic_attr_list should be unchanged from original adds.
    util.verify_attr(fvs_asic, asic_attr_list)

    ####################################
    # Cleanup
    ####################################
    dvs.restart()


class TestP4RTL3MulticastGroup(object):
  """Tests interacting with replication multicast table"""
  def _set_up(self, dvs):
    self._p4rt_l3_multicast_router_intf = (
        l3_multicast.P4RtL3MulticastRouterInterfaceWrapper())
    self._p4rt_l3_multicast_router_intf.set_up_databases(dvs)

    self._p4rt_l3_multicast_group_intf = (
        l3_multicast.P4RtL3MulticastGroupWrapper())
    self._p4rt_l3_multicast_group_intf.set_up_databases(dvs)

    self.p4rt_notifier = swsscommon.NotificationProducer(
      self._p4rt_l3_multicast_group_intf.appl_db,
      swsscommon.APP_P4RT_CHANNEL_NAME)
    self.response_consumer = swsscommon.NotificationConsumer(
      self._p4rt_l3_multicast_group_intf.appl_db, "APPL_DB_" +
      swsscommon.APP_P4RT_TABLE_NAME + "_RESPONSE_CHANNEL")

    self.appl_db_table = (
        self._p4rt_l3_multicast_group_intf.APP_DB_TBL_NAME + ":" +
            self._p4rt_l3_multicast_group_intf.TBL_NAME)
    self.asic_db_group_table = (
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_TBL_NAME)
    self.asic_db_group_member_table = (
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_MEMBER_TBL_NAME)
    self.asic_db_rif_table = (
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME)

  def get_added_multicast_group_oid(self, original_entries):
    """Returns OID key if single multicast group was added"""
    group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_TBL_NAME)
    for key in group_entries:
      if key not in original_entries:
        return key
    return "0"

  def get_added_multicast_group_member_oids(self, original_entries):
    """Returns OID keys of multicast group members added"""
    member_oids = []

    group_member_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_MEMBER_TBL_NAME)
    for key in group_member_entries:
      if key not in original_entries:
        member_oids.append(key)
    return member_oids

  def get_added_rif_oid(self, original_entries):
    """Returns OID key if single RIF was added"""
    rif_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME)

    for key in rif_entries:
      if key not in original_entries:
        return key
    return "0"

  def add_rif(self, port_id=None, instance=None, src_mac=None):
    """Adds a multicast router interface entry"""
    start_asic_db_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_rif_table)

    mcast_router_intf_key, attr_list = (
        self._p4rt_l3_multicast_router_intf.create_router_interface(
            port_id, instance, src_mac))
    util.verify_response(self.response_consumer, mcast_router_intf_key,
                         attr_list, "SWSS_RC_SUCCESS")
    rif_oid = self.get_added_rif_oid(start_asic_db_rif_entries)
    return rif_oid

  def add_and_verify_multicast_group(self, group_id=None, replicas=None,
                                     rif_oids=None, new_replicas=None,
                                     group_oid=None):
    """Adds a multicast group entry and verifies APP DB and ASIC DB"""
    start_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    start_asic_db_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    start_asic_db_group_member_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)
    assert len(rif_oids) == len(new_replicas)

    # Add the group member.
    mcast_group_key, attr_list = (
        self._p4rt_l3_multicast_group_intf.create_multicast_group_entry(
            group_id=group_id, replicas=replicas))
    util.verify_response(self.response_consumer, mcast_group_key,
                         attr_list, "SWSS_RC_SUCCESS")

    # Check that APP DB has expected entry with expected values.
    mcast_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    # If we provided a group_oid, we expected the entry to already exist.
    if group_oid is not None:
      assert len(mcast_group_entries) == len(start_app_db_entries)
    else:
      assert len(mcast_group_entries) == (len(start_app_db_entries) + 1)

    (status, fvs) = util.get_key(
        self._p4rt_l3_multicast_group_intf.appl_db,
        self._p4rt_l3_multicast_group_intf.APP_DB_TBL_NAME,
        mcast_group_key)
    assert status == True
    util.verify_attr(fvs, attr_list)

    # Check that ASIC DB has expected value
    mcast_group_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    if group_oid is not None:
      # If we provided a group_oid, that means we expected the group OID to
      # already exist.
      assert len(mcast_group_asic_entries) == (
          len(start_asic_db_group_entries))
      group_oid_to_ret = group_oid
    else:
      # There are no attributes to check for the group.  We just need to check
      # that there is a new entry.
      assert len(mcast_group_asic_entries) == (
          len(start_asic_db_group_entries) + 1)
      group_oid_to_ret = self.get_added_multicast_group_oid(
          start_asic_db_group_entries)

    # Verify group member.
    mcast_group_member_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)
    assert len(mcast_group_member_asic_entries) == (
        len(start_asic_db_group_member_entries) + len(new_replicas))

    new_group_member_oids = self.get_added_multicast_group_member_oids(
        start_asic_db_group_member_entries)
    assert len(new_group_member_oids) == len(new_replicas)

    for idx, group_member_oid in enumerate(new_group_member_oids):
      (status_asic_group_member, fvs_asic_group_member) = util.get_key(
          self._p4rt_l3_multicast_group_intf.asic_db,
          self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_MEMBER_TBL_NAME,
          group_member_oid)
      assert status_asic_group_member == True

      asic_group_member_attr_list = [
          (self._p4rt_l3_multicast_group_intf.SAI_ATTR_IPMC_GROUP_ID,
           group_oid_to_ret),
          (self._p4rt_l3_multicast_group_intf.SAI_ATTR_IPMC_OUTPUT_ID,
           rif_oids[idx]),
      ]
      util.verify_attr(fvs_asic_group_member, asic_group_member_attr_list)
    return mcast_group_key, attr_list, group_oid_to_ret, new_group_member_oids

  def test_L3MulticastGroupAddUpdateDelete(self, dvs, testlog):
    """
    This test adds a muliticast group member, confirms a group and a member
    were created, confirms an update operation can add a new member, and then
    deletes the group.
    """
    self._set_up(dvs)

    # Fetch database state after init.
    original_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    original_asic_db_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_rif_table)
    original_asic_db_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    original_asic_db_group_member_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)

    # To be able to add multicast groups and members, we need the corresponding
    # router interface to have been created.
    rif_oid_0 = self.add_rif()
    rif_oid_1 = self.add_rif(port_id="Ethernet4")

    ####################################
    # Add operation
    ####################################
    # Add one L3 multicast group entry (one group member).
    mcast_group_key, attr_list, group_oid, group_member_oids = (
        self.add_and_verify_multicast_group(replicas=[("Ethernet8", "0x0")],
                                            rif_oids=[rif_oid_0],
                                            new_replicas=[("Ethernet8", "0x0")]))

    ####################################
    # Update operation
    ####################################
    # We'll add a new replica to the same multicast group.
    mcast_group_key_1, attr_list_1, group_oid_1, group_member_oids_1 = (
        self.add_and_verify_multicast_group(replicas=[("Ethernet8", "0x0"),
                                                      ("Ethernet4", "0x0")],
                                            rif_oids=[rif_oid_1],
                                            new_replicas=[("Ethernet4", "0x0")],
                                            group_oid=group_oid))

    ####################################
    # Delete operation
    ####################################
    self._p4rt_l3_multicast_group_intf.remove_app_db_entry(mcast_group_key)
    util.verify_response(self.response_consumer, mcast_group_key, [],
                         "SWSS_RC_SUCCESS")

    # Check that APP DB entry was removed.
    mcast_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    assert len(mcast_group_entries) == len(original_app_db_entries)

    # Check that ASIC DB entries were removed (both group and member).
    mcast_group_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    assert len(mcast_group_asic_entries) == len(original_asic_db_group_entries)
    mcast_group_member_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)
    assert len(mcast_group_member_asic_entries) == (
        len(original_asic_db_group_member_entries))

    ####################################
    # Cleanup
    ####################################
    dvs.restart()

  def test_L3MulticastGroupAddTwoDeleteOne(self, dvs, testlog):
    """
    This test adds two muliticast group members, confirms the group members were
    created, deletes one group member, and verifies the multicast group remains.
    """
    self._set_up(dvs)

    # Fetch database state after init.
    original_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    original_asic_db_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_rif_table)
    original_asic_db_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    original_asic_db_group_member_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)

    # For this test, we need to setup two RIFs (for the two group members).
    rif_oid_0 = self.add_rif()
    rif_oid_1 = self.add_rif(port_id="Ethernet4")

    ####################################
    # Add operations
    ####################################
    # Add two group members to same multicast group.  We add one replica at a
    # time to allow us to determine which group_member oid was assigned to each.
    mcast_group_key_a, attr_list_a, group_oid_a, group_member_oids_a = (
        self.add_and_verify_multicast_group(replicas=[("Ethernet8", "0x0")],
                                            rif_oids=[rif_oid_0],
                                            new_replicas=[("Ethernet8", "0x0")]))
    mcast_group_key_b, attr_list_b, group_oid_b, group_member_oids_b = (
        self.add_and_verify_multicast_group(replicas=[("Ethernet8", "0x0"),
                                                      ("Ethernet4", "0x0")],
                                            rif_oids=[rif_oid_1],
                                            new_replicas=[("Ethernet4", "0x0")],
                                            group_oid=group_oid_a))

    ####################################
    # Delete group member (via update operation)
    ####################################
    mcast_group_key_1, attr_list_1 = (
        self._p4rt_l3_multicast_group_intf.create_multicast_group_entry(
            replicas=[("Ethernet4", "0x0")]))
    util.verify_response(self.response_consumer, mcast_group_key_1,
                         attr_list_1, "SWSS_RC_SUCCESS")

    # Check that APP DB entry is still there.
    mcast_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    assert len(mcast_group_entries) == (len(original_app_db_entries) + 1)

    # Check that ASIC DB entries were removed (both group and member).
    mcast_group_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    assert len(mcast_group_asic_entries) == (
        len(original_asic_db_group_entries) + 1)
    mcast_group_member_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)
    assert len(mcast_group_member_asic_entries) == (
        len(original_asic_db_group_member_entries) + 1)

    # Confirm that Ethernet4 replica is still there.
    (status_asic_group_member, fvs_asic_group_member) = util.get_key(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        group_member_oids_b[0])
    assert status_asic_group_member == True

    asic_group_member_attr_list = [
        (self._p4rt_l3_multicast_group_intf.SAI_ATTR_IPMC_GROUP_ID,
         group_oid_a),
        (self._p4rt_l3_multicast_group_intf.SAI_ATTR_IPMC_OUTPUT_ID,
         rif_oid_1),
    ]
    util.verify_attr(fvs_asic_group_member, asic_group_member_attr_list)

    ####################################
    # Cleanup
    ####################################
    dvs.restart()

  def test_L3MulticastGroupDeleteUnknown(self, dvs, testlog):
    """
    This test attempts to delete an unknown multicast group.
    """
    self._set_up(dvs)

    # Fetch database state after init.
    original_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    original_asic_db_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    original_asic_db_group_member_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)

    ####################################
    # Delete operation
    ####################################
    mcast_group_key = (
        self._p4rt_l3_multicast_group_intf.generate_app_db_key(
            self._p4rt_l3_multicast_group_intf.DEFAULT_GROUP_ID))

    self._p4rt_l3_multicast_group_intf.remove_app_db_entry(mcast_group_key)
    util.verify_response(
        self.response_consumer, mcast_group_key, [], "SWSS_RC_NOT_FOUND",
        "[OrchAgent] Multicast group entry does not exist for group 0x1")

    # Check that entries remain unchanged.
    mcast_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.appl_db, self.appl_db_table)
    assert len(mcast_app_db_entries) == len(original_app_db_entries)

    mcast_group_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    assert len(mcast_group_asic_entries) == len(original_asic_db_group_entries)
    mcast_group_member_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)
    assert len(mcast_group_member_asic_entries) == (
        len(original_asic_db_group_member_entries))

  def test_L3MulticastGroupAddBeforeRif(self, dvs, testlog):
    """
    This test attempts to add a group member before a RIF was created.
    """
    self._set_up(dvs)

    # Fetch database state after init.
    original_asic_db_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    original_asic_db_group_member_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)

    ####################################
    # Add operation
    ####################################
    # No RIF!
    mcast_group_key, attr_list = (
        self._p4rt_l3_multicast_group_intf.create_multicast_group_entry(
            group_id=None, replicas=[("Ethernet8", "0x0")]))
    util.verify_response(
        self.response_consumer, mcast_group_key, attr_list,
        "SWSS_RC_NOT_FOUND",
        ("[OrchAgent] Multicast group member '0x1:Ethernet8:0x0' "
         "does not have an associated RIF available yet"))

    # Check that asic entries remain unchanged.
    mcast_group_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    assert len(mcast_group_asic_entries) == len(original_asic_db_group_entries)
    mcast_group_member_asic_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_member_table)
    assert len(mcast_group_member_asic_entries) == (
        len(original_asic_db_group_member_entries))


class TestP4RTL3MulticastRoute(object):
  """Tests for interacting with the route tables ipv4_table and ipv6_table"""
  def _set_up(self, dvs):
    self._p4rt_l3_multicast_router_intf = (
        l3_multicast.P4RtL3MulticastRouterInterfaceWrapper())
    self._p4rt_l3_multicast_router_intf.set_up_databases(dvs)

    self._p4rt_l3_multicast_group_intf = (
        l3_multicast.P4RtL3MulticastGroupWrapper())
    self._p4rt_l3_multicast_group_intf.set_up_databases(dvs)

    self._p4rt_l3_multicast_route = l3_multicast.P4RtL3MulticastRouteWrapper()
    self._p4rt_l3_multicast_route.set_up_databases(dvs)

    self.p4rt_notifier = swsscommon.NotificationProducer(
      self._p4rt_l3_multicast_route.appl_db,
      swsscommon.APP_P4RT_CHANNEL_NAME)
    self.response_consumer = swsscommon.NotificationConsumer(
      self._p4rt_l3_multicast_route.appl_db, "APPL_DB_" +
      swsscommon.APP_P4RT_TABLE_NAME + "_RESPONSE_CHANNEL")

    self._vrf_obj = test_vrf.TestVrf()

    self.asic_db_group_table = (
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_TBL_NAME)
    self.asic_db_rif_table = (
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME)

    self.appl_db_table_ipv4 = (
        self._p4rt_l3_multicast_route.APP_DB_TBL_NAME + ":" +
            self._p4rt_l3_multicast_route.TBL_NAME_IPV4)
    self.appl_db_table_ipv6 = (
        self._p4rt_l3_multicast_route.APP_DB_TBL_NAME + ":" +
            self._p4rt_l3_multicast_route.TBL_NAME_IPV6)
    self.asic_db_route_table = self._p4rt_l3_multicast_route.ASIC_DB_TBL_NAME

  def _set_vrf(self, dvs):
    """Sets up a default VRF"""
    self._vrf_obj.setup_db(dvs)
    self.default_vrf_state = self._vrf_obj.vrf_create(
        dvs, self._p4rt_l3_multicast_route.DEFAULT_VRF_ID, [], {})

  def get_added_rif_oid(self, original_entries):
    """Returns OID key if single RIF was added"""
    rif_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self._p4rt_l3_multicast_router_intf.ASIC_DB_TBL_NAME)
    for key in rif_entries:
      if key not in original_entries:
        return key
    return "0"

  def get_added_multicast_group_oid(self, original_entries):
    """Returns OID key if single multicast group was added"""
    group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self._p4rt_l3_multicast_group_intf.ASIC_DB_GROUP_TBL_NAME)
    for key in group_entries:
      if key not in original_entries:
        return key
    return "0"

  def add_rif(self, port_id=None, instance=None, src_mac=None):
    """Adds a multicast router interface entry"""
    start_asic_db_rif_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_rif_table)

    mcast_router_intf_key, attr_list = (
        self._p4rt_l3_multicast_router_intf.create_router_interface(
            port_id, instance, src_mac))
    util.verify_response(self.response_consumer, mcast_router_intf_key,
                         attr_list, "SWSS_RC_SUCCESS")
    rif_oid = self.get_added_rif_oid(start_asic_db_rif_entries)
    return rif_oid

  def add_multicast_group_entry(self, group_id):
    """Adds a multicast group entry with one group member"""
    start_asic_db_group_entries = util.get_keys(
        self._p4rt_l3_multicast_group_intf.asic_db,
        self.asic_db_group_table)
    mcast_group_key, attr_list = (
        self._p4rt_l3_multicast_group_intf.create_multicast_group_entry(
            group_id=group_id))
    util.verify_response(self.response_consumer, mcast_group_key,
                         attr_list, "SWSS_RC_SUCCESS")
    group_oid = self.get_added_multicast_group_oid(start_asic_db_group_entries)
    return group_oid

  def get_added_multicast_asic_route(self, original_entries):
    """Returns asic key if single multicast route entry was added"""
    route_entries = util.get_keys(
        self._p4rt_l3_multicast_route.asic_db,
        self._p4rt_l3_multicast_route.ASIC_DB_TBL_NAME)
    for key in route_entries:
      if key not in original_entries:
        return key
    return "0"

  def get_added_rpf_oid(self):
    """Returns the RPF OID key that was added."""
    # RPF OID is added on the first IPMC entry add.
    rpf_entries = util.get_keys(
        self._p4rt_l3_multicast_route.asic_db,
        self._p4rt_l3_multicast_route.ASIC_DB_RPF_GROUP_TBL_NAME)
    assert len(rpf_entries) == 1
    for key in rpf_entries:
      return key
    return "0"

  def setup_multicast_group(self, group_id=None, add_rif=True):
    """Sets up tables to be able to support a multicast group"""
    if add_rif:
      rif_oid = self.add_rif()
    group_oid = self.add_multicast_group_entry(group_id)
    return group_oid

  def add_and_verify_multicast_route(self, group_id=None, vrf_id=None,
                                     dst_ip=None, is_v4=True, group_oid="0",
                                     rpf_oid=None, update=False,
                                     route_asic_key="0"):
    """Adds a new multicast route entry and verifies APP DB and ASIC DB"""
    if is_v4:
      appl_db_table = self.appl_db_table_ipv4
    else:
      appl_db_table = self.appl_db_table_ipv6

    start_app_db_entries = util.get_keys(
        self._p4rt_l3_multicast_route.appl_db, appl_db_table)
    start_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_route.asic_db,
        self.asic_db_route_table)

    # Add the route entry.
    mcast_route_key, attr_list = (
        self._p4rt_l3_multicast_route.create_multicast_route(
            group_id=group_id, dst_ip=dst_ip, is_v4=is_v4))
    util.verify_response(self.response_consumer, mcast_route_key,
                         attr_list, "SWSS_RC_SUCCESS")
    if update:
      new_entries = 0
    else:
      new_entries = 1

    # Check that APP DB has expected entry with expected values.
    mcast_route_entries = util.get_keys(
        self._p4rt_l3_multicast_route.appl_db, appl_db_table)
    assert len(mcast_route_entries) == (len(start_app_db_entries) + new_entries)

    (status, fvs) = util.get_key(
        self._p4rt_l3_multicast_route.appl_db,
        self._p4rt_l3_multicast_route.APP_DB_TBL_NAME,
        mcast_route_key)
    assert status == True
    util.verify_attr(fvs, attr_list)

    # Check that ASIC DB has expected value
    route_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_route.asic_db,
        self.asic_db_route_table)
    assert len(route_asic_db_entries) == (
        len(start_asic_db_entries) + new_entries)

    # If RPF OID is provided, we expect it to have already been created.
    if rpf_oid is None:
      rpf_oid_to_ret = self.get_added_rpf_oid()
    else:
      rpf_oid_to_ret = rpf_oid

    if update:
      route_asic_key_to_ret = route_asic_key
    else:
      route_asic_key_to_ret = (
          self.get_added_multicast_asic_route(start_asic_db_entries))

    (asic_status, asic_fvs) = util.get_key(
        self._p4rt_l3_multicast_route.asic_db,
        self._p4rt_l3_multicast_route.ASIC_DB_TBL_NAME,
        route_asic_key_to_ret)
    assert asic_status == True
    asic_route_attr_list = [
        (self._p4rt_l3_multicast_route.SAI_ATTR_PACKET_ACTION,
         self._p4rt_l3_multicast_route.SAI_ATTR_PACKET_ACTION_FORWARD),
        (self._p4rt_l3_multicast_route.SAI_ATTR_OUTPUT_GROUP_ID, group_oid),
        (self._p4rt_l3_multicast_route.SAI_ATTR_RPF_GROUP_ID, rpf_oid_to_ret),
    ]
    util.verify_attr(asic_fvs, asic_route_attr_list)
    return mcast_route_key, attr_list, rpf_oid_to_ret, route_asic_key_to_ret

  def test_L3MulticastRouteAddUpdateDelete(self, dvs, testlog):
    """
    This test adds a route entry that assigns the packet to a multicast group,
    confirms that ASIC db is setup, modifies the entry to point to another
    multicast group, confirms ASIC db is setup, and then deletes the route
    entry.
    """
    self._set_up(dvs)
    self._set_vrf(dvs)

    # Fetch database state after init.
    original_app_db_entries_v4 = util.get_keys(
        self._p4rt_l3_multicast_route.appl_db, self.appl_db_table_ipv4)
    original_app_db_entries_v6 = util.get_keys(
        self._p4rt_l3_multicast_route.appl_db, self.appl_db_table_ipv6)
    original_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_route.asic_db, self.asic_db_route_table)

    # Setup multicast groups.
    group_oid_0 = self.setup_multicast_group()
    group_oid_1 = self.setup_multicast_group(group_id="0x2", add_rif=False)

    ####################################
    # Add operation
    ####################################
    # Add two L3 multicast route entries (v4 and v6).
    mcast_route_key_v4, attr_list_v4, rpf_oid, route_asic_key_v4 = (
        self.add_and_verify_multicast_route(group_oid=group_oid_0,
                                            rpf_oid=None))
    mcast_route_key_v6, attr_list_v6, rpf_oid, route_asic_key_v6 = (
        self.add_and_verify_multicast_route(
            group_id="0x2", dst_ip=self._p4rt_l3_multicast_route.DEFAULT_DST_V6,
            is_v4=False, group_oid=group_oid_1, rpf_oid=rpf_oid))

    ####################################
    # Update operation
    ####################################
    # Update v4 route to use multicast group 2 instead of 1.
    mcast_route_key_v4, attr_list_v4, rpf_oid, route_asic_key_v4 = (
        self.add_and_verify_multicast_route(group_id="0x2",
                                            group_oid=group_oid_1,
                                            rpf_oid=rpf_oid, update=True,
                                            route_asic_key=route_asic_key_v4))

    ####################################
    # Delete operation
    ####################################
    self._p4rt_l3_multicast_route.remove_app_db_entry(mcast_route_key_v4)
    util.verify_response(self.response_consumer, mcast_route_key_v4, [],
                         "SWSS_RC_SUCCESS")
    self._p4rt_l3_multicast_route.remove_app_db_entry(mcast_route_key_v6)
    util.verify_response(self.response_consumer, mcast_route_key_v6, [],
                         "SWSS_RC_SUCCESS")

    # Check that APP DB entries were removed.
    route_app_db_entries_v4 = util.get_keys(
        self._p4rt_l3_multicast_route.appl_db, self.appl_db_table_ipv4)
    assert len(route_app_db_entries_v4) == len(original_app_db_entries_v4)
    route_app_db_entries_v6 = util.get_keys(
        self._p4rt_l3_multicast_route.appl_db, self.appl_db_table_ipv6)
    assert len(route_app_db_entries_v6) == len(original_app_db_entries_v6)

    # Check that ASIC DB entries were removed.
    route_asic_db_entries = util.get_keys(
        self._p4rt_l3_multicast_route.asic_db, self.asic_db_route_table)
    assert len(route_asic_db_entries) == len(original_asic_db_entries)

    ####################################
    # Cleanup
    ####################################
    dvs.restart()

  def test_L3MulticastRouteUpdatePacketActionUnimplemented(self, dvs, testlog):
    """
    This test attempts to update a route entry from using the
    set_multicast_group_id action to one of the other table actions.  This is
    not supported and should result in an unimplemented error.
    """
    self._set_up(dvs)
    self._set_vrf(dvs)

    # We need these wrappers so we can properly setup a next hop ID.
    self._p4rt_nexthop_obj = l3.P4RtNextHopWrapper()
    self._p4rt_nexthop_obj.set_up_databases(dvs)
    self._p4rt_router_intf_obj = l3.P4RtRouterInterfaceWrapper()
    self._p4rt_router_intf_obj.set_up_databases(dvs)
    self._p4rt_neighbor_obj = l3.P4RtNeighborWrapper()
    self._p4rt_neighbor_obj.set_up_databases(dvs)

    # Setup multicast groups.
    group_oid_0 = self.setup_multicast_group()

    # Add original route entry that assigns multicast.
    mcast_route_key_v4, attr_list_v4, rpf_oid, route_asic_key_v4 = (
        self.add_and_verify_multicast_route(group_oid=group_oid_0,
                                            rpf_oid=None))

    # Setup items needed for a properly formed next hop update request.
    # Create default router interface for next hop.
    router_interface_id, router_intf_key, attr_list = (
        self._p4rt_router_intf_obj.create_router_interface())
    util.verify_response(
        self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS")
    # Create neighbor.
    neighbor_id, neighbor_key, attr_list = (
        self._p4rt_neighbor_obj.create_neighbor())
    util.verify_response(
        self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS")
    # Create next hop.
    nexthop_id, nexthop_key, attr_list = (
        self._p4rt_nexthop_obj.create_next_hop())
    util.verify_response(
        self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS")

    # Now attempt to update.
    mcast_route_key_v4_update, attr_list_update = (
        self._p4rt_l3_multicast_route.create_multicast_route(
            action=self._p4rt_l3_multicast_route.SET_NEXT_HOP_ID_ACTION,
            param=nexthop_id))
    util.verify_response(
        self.response_consumer, mcast_route_key_v4_update, attr_list_update,
        "SWSS_RC_UNIMPLEMENTED",
        ("[OrchAgent] Changing from action 'set_multicast_group_id' to action "
         "'set_nexthop_id' for entry 'ipv4_dst=10.11.12.0/32:vrf_id=b4-traffic'"
         " is not supported."))

    ####################################
    # Cleanup
    ####################################
    dvs.restart()

  def test_L3MulticastRouteUpdatePacketActionUnimplemented2(self, dvs, testlog):
    """
    This test attempts to update a route entry from using the
    set_nexthop_id action to the set_multicast_group_id action.  This is not
    supported and should result in an unimplemented error.
    """
    self._set_up(dvs)
    self._set_vrf(dvs)

    # We need these wrappers so we can properly setup a next hop ID.
    self._p4rt_nexthop_obj = l3.P4RtNextHopWrapper()
    self._p4rt_nexthop_obj.set_up_databases(dvs)
    self._p4rt_router_intf_obj = l3.P4RtRouterInterfaceWrapper()
    self._p4rt_router_intf_obj.set_up_databases(dvs)
    self._p4rt_neighbor_obj = l3.P4RtNeighborWrapper()
    self._p4rt_neighbor_obj.set_up_databases(dvs)

    # Setup multicast groups.
    group_oid_0 = self.setup_multicast_group()

    # Setup items needed for a properly formed next hop update request.
    # Create default router interface for next hop.
    router_interface_id, router_intf_key, attr_list = (
        self._p4rt_router_intf_obj.create_router_interface())
    util.verify_response(
        self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS")
    # Create neighbor.
    neighbor_id, neighbor_key, attr_list = (
        self._p4rt_neighbor_obj.create_neighbor())
    util.verify_response(
        self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS")
    # Create next hop.
    nexthop_id, nexthop_key, attr_list = (
        self._p4rt_nexthop_obj.create_next_hop())
    util.verify_response(
        self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS")

    # Add original route entry that assigns the next hop.
    mcast_route_key_v4_update, attr_list_update = (
        self._p4rt_l3_multicast_route.create_multicast_route(
            action=self._p4rt_l3_multicast_route.SET_NEXT_HOP_ID_ACTION,
            param=nexthop_id))
    util.verify_response(
        self.response_consumer, mcast_route_key_v4_update, attr_list_update,
        "SWSS_RC_SUCCESS")

    # Now attempt to update.
    mcast_route_key_v4_update, attr_list_update = (
        self._p4rt_l3_multicast_route.create_multicast_route())
    util.verify_response(
        self.response_consumer, mcast_route_key_v4_update, attr_list_update,
        "SWSS_RC_UNIMPLEMENTED",
        ("[OrchAgent] Changing from action 'set_nexthop_id' to action "
         "'set_multicast_group_id' for entry "
         "'ipv4_dst=10.11.12.0/32:vrf_id=b4-traffic' is not supported."))

    ####################################
    # Cleanup
    ####################################
    dvs.restart()

  def test_L3MulticastRouteDeleteUnknown(self, dvs, testlog):
    """
    This test attempts to delete a multicast route entry that does not exist,
    which should result in an error.
    """
    self._set_up(dvs)
    self._set_vrf(dvs)

    mcast_route_key = self._p4rt_l3_multicast_route.generate_app_db_key(
        self._p4rt_l3_multicast_route.DEFAULT_VRF_ID,
        self._p4rt_l3_multicast_route.DEFAULT_DST_V4, ipv6_dst=None)

    self._p4rt_l3_multicast_route.remove_app_db_entry(mcast_route_key)
    util.verify_response(
        self.response_consumer, mcast_route_key, [], "SWSS_RC_NOT_FOUND",
        "[OrchAgent] Route entry does not exist")

    ####################################
    # Cleanup
    ####################################
    dvs.restart()

  def test_L3MulticastRouteAddBeforeGroup(self, dvs, testlog):
    """
    This test attempts to add a multicast route entry before its necessary
    multicast group has been created, which should result in an error.
    """
    self._set_up(dvs)
    self._set_vrf(dvs)

    mcast_route_key_v4_update, attr_list_update = (
        self._p4rt_l3_multicast_route.create_multicast_route())
    util.verify_response(
        self.response_consumer, mcast_route_key_v4_update, attr_list_update,
        "SWSS_RC_NOT_FOUND",
        "[OrchAgent] No multicast group ID found for '0x1'")

    ####################################
    # Cleanup
    ####################################
    dvs.restart()
