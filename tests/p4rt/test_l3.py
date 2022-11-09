from swsscommon import swsscommon

import pytest
import json
import util
import l3
import test_vrf


class TestP4RTL3(object):
    def _set_up(self, dvs):
        self._p4rt_router_intf_obj = l3.P4RtRouterInterfaceWrapper()
        self._p4rt_gre_tunnel_obj = l3.P4RtGreTunnelWrapper()
        self._p4rt_neighbor_obj = l3.P4RtNeighborWrapper()
        self._p4rt_nexthop_obj = l3.P4RtNextHopWrapper()
        self._p4rt_route_obj = l3.P4RtRouteWrapper()
        self._p4rt_wcmp_group_obj = l3.P4RtWcmpGroupWrapper()
        self._vrf_obj = test_vrf.TestVrf()

        self._p4rt_router_intf_obj.set_up_databases(dvs)
        self._p4rt_gre_tunnel_obj.set_up_databases(dvs)
        self._p4rt_neighbor_obj.set_up_databases(dvs)
        self._p4rt_nexthop_obj.set_up_databases(dvs)
        self._p4rt_route_obj.set_up_databases(dvs)
        self._p4rt_wcmp_group_obj.set_up_databases(dvs)
        self.response_consumer = swsscommon.NotificationConsumer(
            self._p4rt_route_obj.appl_db, "APPL_DB_" +
            swsscommon.APP_P4RT_TABLE_NAME + "_RESPONSE_CHANNEL"
        )

    def _set_vrf(self, dvs):
        # Create VRF.
        self._vrf_obj.setup_db(dvs)
        self.vrf_id = "b4-traffic"
        self.vrf_state = self._vrf_obj.vrf_create(dvs, self.vrf_id, [], {})

    def _clean_vrf(self, dvs):
        # Remove VRF.
        self._vrf_obj.vrf_remove(dvs, self.vrf_id, self.vrf_state)

    def test_IPv4RouteWithNexthopAddUpdateDeletePass(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Set IP type for route object.
        self._p4rt_route_obj.set_ip_type("IPV4")

        # Maintain list of original Application and ASIC DB entries before
        # adding new route.
        db_list = (
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)
        db_list = (
            (
                self._p4rt_route_obj.appl_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (
                self._p4rt_route_obj.appl_state_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_route_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor()
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop()
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # get nexthop_oid of newly created nexthop
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create route entry.
        route_key, attr_list = self._p4rt_route_obj.create_route(nexthop_id)
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for newly created route key.
        asic_db_key = self._p4rt_route_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [(self._p4rt_route_obj.SAI_ATTR_NEXTHOP_ID, nexthop_oid)]
        util.verify_attr(fvs, attr_list)

        # Update route entry to set_nexthop_id_and_metadata.
        route_key, attr_list = self._p4rt_route_obj.create_route(
            action="set_nexthop_id_and_metadata", metadata="2"
        )
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count did not change in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        attr_list_appl_db = [
            (self._p4rt_route_obj.ACTION_FIELD, "set_nexthop_id_and_metadata"),
            (
                util.prepend_param_field(
                    self._p4rt_route_obj.NEXTHOP_ID_FIELD),
                nexthop_id,
            ),
            (
                util.prepend_param_field(
                    self._p4rt_route_obj.ROUTE_METADATA_FIELD),
                "2",
            ),
        ]
        util.verify_attr(fvs, attr_list_appl_db)

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for the updated route key.
        asic_db_key = self._p4rt_route_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_route_obj.SAI_ATTR_NEXTHOP_ID, nexthop_oid),
            (self._p4rt_route_obj.SAI_ATTR_META_DATA, "2"),
        ]
        util.verify_attr(fvs, attr_list)

        # Update route entry to drop.
        route_key, attr_list = self._p4rt_route_obj.create_route(action="drop")
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count did not change in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        attr_list_appl_db = [
            (self._p4rt_route_obj.ACTION_FIELD, "drop"),
            (
                util.prepend_param_field(
                    self._p4rt_route_obj.NEXTHOP_ID_FIELD),
                nexthop_id,
            ),
            (
                util.prepend_param_field(
                    self._p4rt_route_obj.ROUTE_METADATA_FIELD),
                "2",
            ),
        ]
        util.verify_attr(fvs, attr_list_appl_db)

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for the updated route key.
        asic_db_key = self._p4rt_route_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_route_obj.SAI_ATTR_NEXTHOP_ID, "oid:0x0"),
            (
                self._p4rt_route_obj.SAI_ATTR_PACKET_ACTION,
                self._p4rt_route_obj.SAI_ATTR_PACKET_ACTION_DROP,
            ),
            (self._p4rt_route_obj.SAI_ATTR_META_DATA, "0"),
        ]
        util.verify_attr(fvs, attr_list)

        # Remove route entry.
        self._p4rt_route_obj.remove_app_db_entry(route_key)
        util.verify_response(self.response_consumer,
                             route_key, [], "SWSS_RC_SUCCESS")

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove nexthop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)
        util.verify_response(self.response_consumer,
                             nexthop_key, [], "SWSS_RC_SUCCESS")

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)
        util.verify_response(
            self.response_consumer, neighbor_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)
        util.verify_response(
            self.response_consumer, router_intf_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count is same as the original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count()
        )

        # Verify that the route_key no longer exists in application database.
        (status, fsv) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == False

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that the route_key no longer exists in application state
        # database.
        (status, fsv) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == False

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count()
        )

        # Verify that removed route no longer exists in ASIC database.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == False
        self._clean_vrf(dvs)

    def test_IPv6WithWcmpRouteAddUpdateDeletePass(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Set IP type for route object.
        self._p4rt_route_obj.set_ip_type("IPV6")

        # Maintain list of original Application and ASIC DB entries before
        # adding new route.
        db_list = (
            (
                self._p4rt_route_obj.appl_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (
                self._p4rt_route_obj.appl_state_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_route_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)
        db_list = (
            (
                self._p4rt_wcmp_group_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            ),
        )
        self._p4rt_wcmp_group_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor(
            ipv4=False
        )
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop(
            ipv4=False
        )
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # Get the oid of the newly created nexthop.
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create wcmp group.
        (
            wcmp_group_id,
            wcmp_group_key,
            attr_list,
        ) = self._p4rt_wcmp_group_obj.create_wcmp_group()
        util.verify_response(
            self.response_consumer, wcmp_group_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count += 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for wcmp group entries.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count() + 1
        )

        # Query ASIC database for newly created wcmp group oid.
        wcmp_group_oid = self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_oid()
        assert wcmp_group_oid is not None
        attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_TYPE,
                self._p4rt_wcmp_group_obj.SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP,
            )
        ]
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            wcmp_group_oid,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for wcmp group member entries.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count() + 1
        )

        # Query ASIC database for newly crated wcmp group member key.
        asic_db_group_member_key = (
            self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_member_asic_db_key()
        )
        assert asic_db_group_member_key is not None
        attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_GROUP_ID,
                wcmp_group_oid,
            ),
            (self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_ID, nexthop_oid),
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_WEIGHT,
                str(self._p4rt_wcmp_group_obj.DEFAULT_WEIGHT),
            ),
        ]
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            asic_db_group_member_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Create route entry.
        route_key, attr_list = self._p4rt_route_obj.create_route(
            wcmp_group_id=wcmp_group_id, action="set_wcmp_group_id", dst="2001:db8::/32"
        )
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for newly created route key.
        asic_db_key = self._p4rt_route_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_route_obj.SAI_ATTR_NEXTHOP_ID, wcmp_group_oid)]
        util.verify_attr(fvs, attr_list)

        # Update route entry to drop action
        route_key, attr_list = self._p4rt_route_obj.create_route(
            action="drop", dst="2001:db8::/32"
        )
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count did not change in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        attr_list_appl_db = [
            (self._p4rt_route_obj.ACTION_FIELD, "drop"),
            (
                util.prepend_param_field(
                    self._p4rt_route_obj.WCMP_GROUP_ID_FIELD),
                wcmp_group_id,
            ),
        ]
        util.verify_attr(fvs, attr_list_appl_db)

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for the updated route key.
        asic_db_key = self._p4rt_route_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_route_obj.SAI_ATTR_NEXTHOP_ID, "oid:0x0"),
            (
                self._p4rt_route_obj.SAI_ATTR_PACKET_ACTION,
                self._p4rt_route_obj.SAI_ATTR_PACKET_ACTION_DROP,
            ),
        ]
        util.verify_attr(fvs, attr_list)

        # Update route entry to trap action.
        route_key, attr_list = self._p4rt_route_obj.create_route(
            action="trap", dst="2001:db8::/32"
        )
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count did not change in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        attr_list_appl_db = [
            (self._p4rt_route_obj.ACTION_FIELD, "trap"),
            (
                util.prepend_param_field(
                    self._p4rt_route_obj.WCMP_GROUP_ID_FIELD),
                wcmp_group_id,
            ),
        ]
        util.verify_attr(fvs, attr_list_appl_db)

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for the updated route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for the updated route key.
        asic_db_key = self._p4rt_route_obj.get_newly_created_asic_db_key()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_route_obj.SAI_ATTR_NEXTHOP_ID, "oid:0x0"),
            (
                self._p4rt_route_obj.SAI_ATTR_PACKET_ACTION,
                self._p4rt_route_obj.SAI_ATTR_PACKET_ACTION_TRAP,
            ),
        ]
        util.verify_attr(fvs, attr_list)

        # Remove route entry.
        self._p4rt_route_obj.remove_app_db_entry(route_key)
        util.verify_response(self.response_consumer,
                             route_key, [], "SWSS_RC_SUCCESS")

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove wcmp group entry.
        self._p4rt_wcmp_group_obj.remove_app_db_entry(wcmp_group_key)
        util.verify_response(
            self.response_consumer, wcmp_group_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count decremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count -= 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove nexthop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)
        util.verify_response(self.response_consumer,
                             nexthop_key, [], "SWSS_RC_SUCCESS")

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)
        util.verify_response(
            self.response_consumer, neighbor_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)
        util.verify_response(
            self.response_consumer, router_intf_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count is same as original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count()
        )

        # Verify that the route_key no longer exists in application database.
        (status, fsv) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == False

        # Query application state database for route entries.
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that the route_key no longer exists in application state
        # database.
        (status, fsv) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == False

        # Query ASIC database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count()
        )

        # Verify that removed route no longer exists in ASIC database.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.asic_db,
            self._p4rt_route_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == False

        # Query application database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_db_entries_count()
        )

        # Verify that the route_key no longer exists in application database.
        (status, fsv) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == False

        # Query application state database for wcmp group entries.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that the wcmp_group_key no longer exists in application state
        # database.
        (status, fsv) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == False

        # Query ASIC database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count()
        )

        # Verify that removed wcmp group no longer exists in ASIC database.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            wcmp_group_oid,
        )
        assert status == False

        # Query ASIC database for wcmp group member entries.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count()
        )

        # Verify that removed wcmp group member no longer exists in ASIC
        # database.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            asic_db_group_member_key,
        )
        assert status == False

        self._clean_vrf(dvs)

    def test_NexthopWithGreTunnelAddDeletePass(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Maintain list of original Application and ASIC DB entries before
        # adding new entries.
        db_list = (
            (
                self._p4rt_nexthop_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
                    self._p4rt_nexthop_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_nexthop_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
                    self._p4rt_nexthop_obj.TBL_NAME,
                ),
            ),
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)
        db_list = (
            (
                self._p4rt_gre_tunnel_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
                    self._p4rt_gre_tunnel_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_gre_tunnel_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
                    self._p4rt_gre_tunnel_obj.TBL_NAME,
                ),
            ),
            (self._p4rt_gre_tunnel_obj.asic_db,
             self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_gre_tunnel_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_router_intf_obj.asic_db,
             self._p4rt_router_intf_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_router_intf_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # get router_interface_oid of newly created router_intf
        router_intf_oid = self._p4rt_router_intf_obj.get_newly_created_router_interface_oid()
        assert router_intf_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create tunnel.
        tunnel_id, tunnel_key, attr_list = self._p4rt_gre_tunnel_obj.create_gre_tunnel()
        util.verify_response(
            self.response_consumer, tunnel_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # get tunnel_oid of newly created tunnel
        tunnel_oid = self._p4rt_gre_tunnel_obj.get_newly_created_tunnel_oid()
        assert tunnel_oid is not None
        # get overlay router_interface_oid of newly created router_intf
        overlay_router_intf_oid = self._p4rt_router_intf_obj.get_newly_created_router_interface_oid(
            set([router_intf_oid]))
        assert overlay_router_intf_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for tunnel entries.
        tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.appl_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_gre_tunnel_obj.TBL_NAME,
        )
        assert len(tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created tunnel key.
        (status, fvs) = util.get_key(
            self._p4rt_gre_tunnel_obj.appl_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
            tunnel_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for tunnel entries.
        state_tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.appl_state_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_gre_tunnel_obj.TBL_NAME,
        )
        assert len(state_tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created tunnel key.
        (status, fvs) = util.get_key(
            self._p4rt_gre_tunnel_obj.appl_state_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
            tunnel_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for tunnel entries.
        tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.asic_db, self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME
        )
        assert len(tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for newly created nexthop key.
        asic_db_key = self._p4rt_gre_tunnel_obj.get_newly_created_tunnel_oid()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_gre_tunnel_obj.asic_db,
            self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_gre_tunnel_obj.SAI_ATTR_UNDERLAY_INTERFACE, router_intf_oid),
            (self._p4rt_gre_tunnel_obj.SAI_ATTR_OVERLAY_INTERFACE,
             overlay_router_intf_oid),
            (self._p4rt_gre_tunnel_obj.SAI_ATTR_TYPE, "SAI_TUNNEL_TYPE_IPINIP_GRE"),
            (self._p4rt_gre_tunnel_obj.SAI_ATTR_PEER_MODE, "SAI_TUNNEL_PEER_MODE_P2P"),
            (self._p4rt_gre_tunnel_obj.SAI_ATTR_ENCAP_SRC_IP,
             self._p4rt_gre_tunnel_obj.DEFAULT_ENCAP_SRC_IP),
            (self._p4rt_gre_tunnel_obj.SAI_ATTR_ENCAP_DST_IP,
             self._p4rt_gre_tunnel_obj.DEFAULT_ENCAP_DST_IP),
        ]
        util.verify_attr(fvs, attr_list)

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor()
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create tunnel nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop(
            tunnel_id=tunnel_id
        )
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # get nexthop_oid of newly created nexthop
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for nexthop entries.
        nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.appl_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME + ":" + self._p4rt_nexthop_obj.TBL_NAME,
        )
        assert len(nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created nexthop key.
        (status, fvs) = util.get_key(
            self._p4rt_nexthop_obj.appl_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
            nexthop_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for nexthop entries.
        state_nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.appl_state_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME + ":" + self._p4rt_nexthop_obj.TBL_NAME,
        )
        assert len(state_nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created nexthop key.
        (status, fvs) = util.get_key(
            self._p4rt_nexthop_obj.appl_state_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
            nexthop_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for nexthop entries.
        nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.asic_db, self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME
        )
        assert len(nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_asic_db_entries_count() + 1
        )

        # Query ASIC database for newly created nexthop key.
        asic_db_key = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert asic_db_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_nexthop_obj.asic_db,
            self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == True
        attr_list = [
            (self._p4rt_nexthop_obj.SAI_ATTR_TUNNEL_OID, tunnel_oid),
            (self._p4rt_nexthop_obj.SAI_ATTR_IP,
             self._p4rt_nexthop_obj.DEFAULT_IPV4_NEIGHBOR_ID),
            (self._p4rt_nexthop_obj.SAI_ATTR_TYPE,
             self._p4rt_nexthop_obj.SAI_ATTR_TUNNEL_ENCAP)
        ]
        util.verify_attr(fvs, attr_list)

        # Remove nexthop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)
        util.verify_response(self.response_consumer,
                             nexthop_key, [], "SWSS_RC_SUCCESS")

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)
        util.verify_response(
            self.response_consumer, neighbor_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove tunnel.
        self._p4rt_gre_tunnel_obj.remove_app_db_entry(tunnel_key)
        util.verify_response(
            self.response_consumer, tunnel_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Remove router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)
        util.verify_response(
            self.response_consumer, router_intf_key, [], "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count is same as the original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

        # Query application database for nexthop entries.
        nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.appl_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME + ":" + self._p4rt_nexthop_obj.TBL_NAME,
        )
        assert len(nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_appl_db_entries_count()
        )

        # Verify that the nexthop_key no longer exists in application database.
        (status, fsv) = util.get_key(
            self._p4rt_nexthop_obj.appl_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
            nexthop_key,
        )
        assert status == False

        # Query application state database for nexthop entries.
        state_nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.appl_state_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME + ":" + self._p4rt_nexthop_obj.TBL_NAME,
        )
        assert len(state_nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that the nexthop_key no longer exists in application state
        # database.
        (status, fsv) = util.get_key(
            self._p4rt_nexthop_obj.appl_state_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
            nexthop_key,
        )
        assert status == False

        # Query ASIC database for nexthop entries.
        nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.asic_db, self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME
        )
        assert len(nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_asic_db_entries_count()
        )

        # Verify that removed nexthop no longer exists in ASIC database.
        (status, fvs) = util.get_key(
            self._p4rt_nexthop_obj.asic_db,
            self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == False

        # Query application database for tunnel entries.
        tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.appl_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_gre_tunnel_obj.TBL_NAME,
        )
        assert len(tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_appl_db_entries_count()
        )

        # Verify that the tunnel_key no longer exists in application database.
        (status, fsv) = util.get_key(
            self._p4rt_gre_tunnel_obj.appl_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
            tunnel_key,
        )
        assert status == False

        # Query application state database for tunnel entries.
        state_tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.appl_state_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_gre_tunnel_obj.TBL_NAME,
        )
        assert len(state_tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that the tunnel_key no longer exists in application state
        # database.
        (status, fsv) = util.get_key(
            self._p4rt_gre_tunnel_obj.appl_state_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
            tunnel_key,
        )
        assert status == False

        # Query ASIC database for tunnel entries.
        runnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.asic_db, self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME
        )
        assert len(tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_asic_db_entries_count()
        )

        # Verify that removed route no longer exists in ASIC database.
        (status, fvs) = util.get_key(
            self._p4rt_gre_tunnel_obj.asic_db,
            self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME,
            asic_db_key,
        )
        assert status == False
        self._clean_vrf(dvs)

    def test_IPv4RouteAddWithInvalidNexthopFail(self, dvs, testlog):
        marker = dvs.add_log_marker()

        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Set IP type for route object.
        self._p4rt_route_obj.set_ip_type("IPV4")

        # Maintain list of original Application and ASIC DB entries before
        # adding new route.
        db_list = (
            (
                self._p4rt_route_obj.appl_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (
                self._p4rt_route_obj.appl_state_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_route_obj.get_original_redis_entries(db_list)

        # Create route entry using invalid nexthop (expect failure).
        route_key, attr_list = self._p4rt_route_obj.create_route()
        err_log = "[OrchAgent] Nexthop ID '8' does not exist"
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_NOT_FOUND", err_log
        )

        # Query application database for route entries.
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application database for route entries (no new route entry
        # expected).
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that the newly added route key does not exist in application
        # state db.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == False

        # Query ASIC database for route entries (no new ASIC DB entry should be
        # created for route entry).
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count()
        )

        # Remove route entry (expect failure).
        self._p4rt_route_obj.remove_app_db_entry(route_key)
        err_log = "[OrchAgent] Route entry does not exist"
        util.verify_response(
            self.response_consumer, route_key, [], "SWSS_RC_NOT_FOUND", err_log
        )
        self._clean_vrf(dvs)

    def test_IPv6RouteAddWithInvalidWcmpFail(self, dvs, testlog):
        marker = dvs.add_log_marker()

        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Set IP type for route object.
        self._p4rt_route_obj.set_ip_type("IPV6")

        # Maintain list of original Application and ASIC DB entries before
        # adding new route.
        db_list = (
            (
                self._p4rt_route_obj.appl_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (
                self._p4rt_route_obj.appl_state_db,
                "%s:%s"
                % (self._p4rt_route_obj.APP_DB_TBL_NAME, self._p4rt_route_obj.TBL_NAME),
            ),
            (self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_route_obj.get_original_redis_entries(db_list)

        # Create route entry using invalid wcmp group (expect failure).
        route_key, attr_list = self._p4rt_route_obj.create_route(
            action="set_wcmp_group_id", wcmp_group_id="8"
        )
        err_log = "[OrchAgent] WCMP group '8' does not exist"
        util.verify_response(
            self.response_consumer, route_key, attr_list, "SWSS_RC_NOT_FOUND", err_log
        )

        # Query application database for route entries
        route_entries = util.get_keys(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created route key.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for route entries (no new APPL STATE DB
        # entry should be created for route entry).
        state_route_entries = util.get_keys(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME + ":" + self._p4rt_route_obj.TBL_NAME,
        )
        assert len(state_route_entries) == (
            self._p4rt_route_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that newly created route key does not exist in application
        # state db.
        (status, fvs) = util.get_key(
            self._p4rt_route_obj.appl_state_db,
            self._p4rt_route_obj.APP_DB_TBL_NAME,
            route_key,
        )
        assert status == False

        # Query ASIC database for route entries (no new ASIC DB entry should be
        # created for route entry).
        route_entries = util.get_keys(
            self._p4rt_route_obj.asic_db, self._p4rt_route_obj.ASIC_DB_TBL_NAME
        )
        assert len(route_entries) == (
            self._p4rt_route_obj.get_original_asic_db_entries_count()
        )

        # Remove route entry (expect failure).
        self._p4rt_route_obj.remove_app_db_entry(route_key)
        err_log = "[OrchAgent] Route entry does not exist"
        util.verify_response(
            self.response_consumer, route_key, [], "SWSS_RC_NOT_FOUND", err_log
        )
        self._clean_vrf(dvs)

    def test_PruneAndRestoreNextHop(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # Maintain original WCMP group entries for ASIC DB.
        db_list = (
            (
                self._p4rt_wcmp_group_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            ),
        )
        self._p4rt_wcmp_group_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Bring up port under test.
        port_name = "Ethernet0"
        if_name = "eth0"
        util.initialize_interface(dvs, port_name, "10.0.0.0/31")
        util.set_interface_status(dvs, if_name, "up")

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor()
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop()
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # Get nexthop_oid of newly created nexthop.
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create wcmp group with one member.
        (
            wcmp_group_id,
            wcmp_group_key,
            attr_list,
        ) = self._p4rt_wcmp_group_obj.create_wcmp_group(watch_port=port_name)
        util.verify_response(
            self.response_consumer, wcmp_group_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count += 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for wcmp group entries.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count() + 1
        )

        # Query ASIC database for newly created wcmp group oid.
        wcmp_group_oid = self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_oid()
        assert wcmp_group_oid is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            wcmp_group_oid,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_TYPE,
                (
                    self._p4rt_wcmp_group_obj.SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP
                ),
            )
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Query ASIC database for newly created wcmp group member key.
        asic_db_group_member_key = (
            self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_member_asic_db_key()
        )
        assert asic_db_group_member_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            asic_db_group_member_key,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_GROUP_ID,
                wcmp_group_oid,
            ),
            (self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_ID, nexthop_oid),
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_WEIGHT,
                str(self._p4rt_wcmp_group_obj.DEFAULT_WEIGHT),
            ),
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Force oper-down for the associated port.
        util.set_interface_status(dvs, if_name)

        # Check ASIC DB to verify that associated member for watch_port is
        # pruned.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count()
        )

        # Check APPL STATE DB to verify no change.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Force oper-up for associated port.
        util.set_interface_status(dvs, if_name, "up")

        # Check pruned next hop member is restored in ASIC DB.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count() + 1
        )
        asic_db_group_member_key = (
            self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_member_asic_db_key()
        )
        assert asic_db_group_member_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            asic_db_group_member_key,
        )
        assert status == True
        util.verify_attr(fvs, asic_attr_list)

        # Delete WCMP group member.
        self._p4rt_wcmp_group_obj.remove_app_db_entry(wcmp_group_key)

        # Verify that P4RT key to OID count decremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count -= 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Verify that APPL STATE DB is now updated.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            (
                self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
                + ":"
                + self._p4rt_wcmp_group_obj.TBL_NAME
            ),
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count()
        )

        # Delete next hop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)

        # Verify that P4RT key to OID count is same as the original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

    def test_PruneNextHopOnWarmBoot(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # Maintain original WCMP group entries for ASIC DB.
        db_list = (
            (
                self._p4rt_wcmp_group_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            ),
        )
        self._p4rt_wcmp_group_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Bring up port under test.
        port_name = "Ethernet0"
        if_name = "eth0"
        util.initialize_interface(dvs, port_name, "10.0.0.0/31")
        util.set_interface_status(dvs, if_name, "up")

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor()
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop()
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # Get nexthop_oid of newly created nexthop.
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create wcmp group with one member.
        (
            wcmp_group_id,
            wcmp_group_key,
            attr_list,
        ) = self._p4rt_wcmp_group_obj.create_wcmp_group(watch_port=port_name)
        util.verify_response(
            self.response_consumer, wcmp_group_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count += 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for wcmp group entries.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count() + 1
        )

        # Query ASIC database for newly created wcmp group oid.
        wcmp_group_oid = self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_oid()
        assert wcmp_group_oid is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            wcmp_group_oid,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_TYPE,
                (
                    self._p4rt_wcmp_group_obj.SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP
                ),
            )
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Query ASIC database for wcmp group member entries.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count() + 1
        )

        # Query ASIC database for newly created wcmp group member key.
        asic_db_group_member_key = (
            self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_member_asic_db_key()
        )
        assert asic_db_group_member_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            asic_db_group_member_key,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_GROUP_ID,
                wcmp_group_oid,
            ),
            (self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_ID, nexthop_oid),
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_WEIGHT,
                str(self._p4rt_wcmp_group_obj.DEFAULT_WEIGHT),
            ),
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Bring down the port.
        util.set_interface_status(dvs, if_name)

        # Execute the warm reboot.
        dvs.warm_restart_swss("true")
        dvs.stop_swss()
        dvs.start_swss()

        # Make sure the system is stable.
        dvs.check_swss_ready()

        # Verify that the associated next hop is pruned in ASIC DB.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count()
        )

        # Delete WCMP group member.
        self._p4rt_wcmp_group_obj.remove_app_db_entry(wcmp_group_key)

        # Verify that P4RT key to OID count decremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count -= 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Verify that APPL STATE DB is updated.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            (
                self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
                + ":"
                + self._p4rt_wcmp_group_obj.TBL_NAME
            ),
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count()
        )

        # Delete next hop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)

        # Verify that P4RT key to OID count is same as the original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

    def test_CreateWcmpMemberForOperUpWatchportOnly(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # Maintain original WCMP group entries for ASIC DB.
        db_list = (
            (
                self._p4rt_wcmp_group_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            ),
        )
        self._p4rt_wcmp_group_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Force oper-down on port under test.
        port_name = "Ethernet0"
        if_name = "eth0"
        util.initialize_interface(dvs, port_name, "10.0.0.0/31")
        util.set_interface_status(dvs, if_name)

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor()
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop()
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # Get nexthop_oid of newly created nexthop.
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create wcmp group with one member.
        (
            wcmp_group_id,
            wcmp_group_key,
            attr_list,
        ) = self._p4rt_wcmp_group_obj.create_wcmp_group(watch_port=port_name)
        util.verify_response(
            self.response_consumer, wcmp_group_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB
        # (WCMP group member is not created for operationally down watchport).
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for wcmp group entries.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count() + 1
        )

        # Query ASIC database for newly created wcmp group oid.
        wcmp_group_oid = self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_oid()
        assert wcmp_group_oid is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            wcmp_group_oid,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_TYPE,
                (
                    self._p4rt_wcmp_group_obj.SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP
                ),
            )
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Query ASIC database for wcmp group member entries (expect no entry).
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count()
        )

        # Bring up the port.
        util.set_interface_status(dvs, if_name, "up")

        # Verify that P4RT key to OID count incremented by 1 in Redis DB
        # (WCMP group member is now expected to be created in SAI due to
        # watchport now being operationally up)
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Verify that next hop member is now created in SAI.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count() + 1
        )
        asic_db_group_member_key = (
            self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_member_asic_db_key()
        )
        assert asic_db_group_member_key is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            (self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME),
            asic_db_group_member_key,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_GROUP_ID,
                wcmp_group_oid,
            ),
            (self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_NEXTHOP_ID, nexthop_oid),
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_MEMBER_WEIGHT,
                str(self._p4rt_wcmp_group_obj.DEFAULT_WEIGHT),
            ),
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Delete WCMP group member.
        self._p4rt_wcmp_group_obj.remove_app_db_entry(wcmp_group_key)

        # Verify that P4RT key to OID count decremented by 2 in Redis DB
        # (1 each for WCMP group and member).
        count -= 2
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Verify that APPL STATE DB is updated.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            (
                self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
                + ":"
                + self._p4rt_wcmp_group_obj.TBL_NAME
            ),
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count()
        )

        # Delete next hop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)

        # Verify that P4RT key to OID count is same as the original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

    def test_RemovePrunedWcmpGroupMember(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # Maintain original WCMP group entries for ASIC DB.
        db_list = (
            (
                self._p4rt_wcmp_group_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
                    self._p4rt_wcmp_group_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            ),
            (
                self._p4rt_wcmp_group_obj.asic_db,
                self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
            ),
        )
        self._p4rt_wcmp_group_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Force oper-down on port under test.
        port_name = "Ethernet0"
        if_name = "eth0"
        util.initialize_interface(dvs, port_name, "10.0.0.0/31")
        util.set_interface_status(dvs, if_name)

        # Create router interface.
        (
            router_interface_id,
            router_intf_key,
            attr_list,
        ) = self._p4rt_router_intf_obj.create_router_interface()
        util.verify_response(
            self.response_consumer, router_intf_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count = 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = self._p4rt_neighbor_obj.create_neighbor()
        util.verify_response(
            self.response_consumer, neighbor_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop()
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_SUCCESS"
        )
        # Get nexthop_oid of newly created nexthop.
        nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert nexthop_oid is not None

        # Verify that P4RT key to OID count incremented by 1 in Redis DB.
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Create wcmp group with one member.
        (
            wcmp_group_id,
            wcmp_group_key,
            attr_list,
        ) = self._p4rt_wcmp_group_obj.create_wcmp_group(watch_port=port_name)
        util.verify_response(
            self.response_consumer, wcmp_group_key, attr_list, "SWSS_RC_SUCCESS"
        )

        # Verify that P4RT key to OID count incremented by 1 in Redis DB
        # (WCMP group member is not created for operationally down watchport).
        count += 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Query application database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query application state database for wcmp group entries.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
            + ":"
            + self._p4rt_wcmp_group_obj.TBL_NAME,
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count() + 1
        )

        # Query application state database for newly created wcmp group key.
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.appl_state_db,
            self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME,
            wcmp_group_key,
        )
        assert status == True
        util.verify_attr(fvs, attr_list)

        # Query ASIC database for wcmp group entries.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count() + 1
        )

        # Query ASIC database for newly created wcmp group oid.
        wcmp_group_oid = self._p4rt_wcmp_group_obj.get_newly_created_wcmp_group_oid()
        assert wcmp_group_oid is not None
        (status, fvs) = util.get_key(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
            wcmp_group_oid,
        )
        assert status == True
        asic_attr_list = [
            (
                self._p4rt_wcmp_group_obj.SAI_ATTR_GROUP_TYPE,
                (
                    self._p4rt_wcmp_group_obj.SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP
                ),
            )
        ]
        util.verify_attr(fvs, asic_attr_list)

        # Query ASIC database for wcmp group member entries.
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count() + 1
        )

        # Query ASIC database for wcmp group member entries (expect no entry).
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert (
            len(wcmp_group_member_entries)
            == self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count()
        )

        # Attempt to delete the next hop. Expect failure as the pruned WCMP
        # group member is still referencing it.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)

        # Verify that the P4RT key to OID count is same as before in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Verify that the next hop still exists in app state db.
        (status, fvs) = util.get_key(
            self._p4rt_nexthop_obj.appl_state_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
            nexthop_key,
        )
        assert status == True

        # Delete the pruned wcmp group member and try again.
        self._p4rt_wcmp_group_obj.remove_app_db_entry(wcmp_group_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Verify that APPL STATE DB is updated.
        state_wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.appl_state_db,
            (
                self._p4rt_wcmp_group_obj.APP_DB_TBL_NAME
                + ":"
                + self._p4rt_wcmp_group_obj.TBL_NAME
            ),
        )
        assert len(state_wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_appl_state_db_entries_count()
        )

        # Verify that ASIC DB is updated.
        wcmp_group_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_TBL_NAME,
        )
        assert len(wcmp_group_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_group_entries_count()
        )
        wcmp_group_member_entries = util.get_keys(
            self._p4rt_wcmp_group_obj.asic_db,
            self._p4rt_wcmp_group_obj.ASIC_DB_GROUP_MEMBER_TBL_NAME,
        )
        assert len(wcmp_group_member_entries) == (
            self._p4rt_wcmp_group_obj.get_original_asic_db_member_entries_count()
        )

        # Delete next hop.
        self._p4rt_nexthop_obj.remove_app_db_entry(nexthop_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete neighbor.
        self._p4rt_neighbor_obj.remove_app_db_entry(neighbor_key)

        # Verify that P4RT key to OID count decremented by 1 in Redis DB.
        count -= 1
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == True
        assert len(fvs) == len(original_key_oid_info) + count

        # Delete router interface.
        self._p4rt_router_intf_obj.remove_app_db_entry(router_intf_key)

        # Verify that P4RT key to OID count is same as the original count.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

    def test_NexthopWithGreTunnelCreationFailIfDependenciesAreMissing(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        self._set_vrf(dvs)

        # Maintain list of original Application and ASIC DB entries before
        # adding new entries.
        db_list = (
            (
                self._p4rt_nexthop_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
                    self._p4rt_nexthop_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_nexthop_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_nexthop_obj.APP_DB_TBL_NAME,
                    self._p4rt_nexthop_obj.TBL_NAME,
                ),
            ),
            (self._p4rt_nexthop_obj.asic_db,
             self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)
        db_list = (
            (
                self._p4rt_gre_tunnel_obj.appl_db,
                "%s:%s"
                % (
                    self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
                    self._p4rt_gre_tunnel_obj.TBL_NAME,
                ),
            ),
            (
                self._p4rt_gre_tunnel_obj.appl_state_db,
                "%s:%s"
                % (
                    self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME,
                    self._p4rt_gre_tunnel_obj.TBL_NAME,
                ),
            ),
            (self._p4rt_gre_tunnel_obj.asic_db,
             self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_gre_tunnel_obj.get_original_redis_entries(db_list)
        db_list = (
            (self._p4rt_router_intf_obj.asic_db,
             self._p4rt_router_intf_obj.ASIC_DB_TBL_NAME),
        )
        self._p4rt_router_intf_obj.get_original_redis_entries(db_list)

        # Fetch the original key to oid information from Redis DB.
        key_to_oid_helper = util.KeyToOidDBHelper(dvs)
        _, original_key_oid_info = key_to_oid_helper.get_db_info()

        # Create tunnel.
        tunnel_id, tunnel_key, attr_list = self._p4rt_gre_tunnel_obj.create_gre_tunnel()
        util.verify_response(
            self.response_consumer, tunnel_key, attr_list, "SWSS_RC_NOT_FOUND",
            "[OrchAgent] Router intf '16' does not exist"
        )

        # Verify that P4RT key to OID count does not change in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

        # Query application database for tunnel entries.
        tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.appl_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_gre_tunnel_obj.TBL_NAME,
        )
        assert len(tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application state database for tunnel entries.
        state_tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.appl_state_db,
            self._p4rt_gre_tunnel_obj.APP_DB_TBL_NAME +
            ":" + self._p4rt_gre_tunnel_obj.TBL_NAME,
        )
        assert len(state_tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_appl_state_db_entries_count()
        )

        # Query ASIC database for tunnel entries.
        tunnel_entries = util.get_keys(
            self._p4rt_gre_tunnel_obj.asic_db, self._p4rt_gre_tunnel_obj.ASIC_DB_TBL_NAME
        )
        assert len(tunnel_entries) == (
            self._p4rt_gre_tunnel_obj.get_original_asic_db_entries_count()
        )

        # Create tunnel nexthop.
        nexthop_id, nexthop_key, attr_list = self._p4rt_nexthop_obj.create_next_hop(
            tunnel_id=tunnel_id
        )
        util.verify_response(
            self.response_consumer, nexthop_key, attr_list, "SWSS_RC_NOT_FOUND",
            "[OrchAgent] GRE Tunnel 'tunnel-1' does not exist in GRE Tunnel Manager"
        )

        # Verify that P4RT key to OID count does not change in Redis DB.
        status, fvs = key_to_oid_helper.get_db_info()
        assert status == False
        assert len(fvs) == len(original_key_oid_info)

        # Query application database for nexthop entries.
        nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.appl_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME + ":" + self._p4rt_nexthop_obj.TBL_NAME,
        )
        assert len(nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application state database for nexthop entries.
        state_nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.appl_state_db,
            self._p4rt_nexthop_obj.APP_DB_TBL_NAME + ":" + self._p4rt_nexthop_obj.TBL_NAME,
        )
        assert len(state_nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_appl_state_db_entries_count()
        )

        # Query ASIC database for nexthop entries.
        nexthop_entries = util.get_keys(
            self._p4rt_nexthop_obj.asic_db, self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME
        )
        assert len(nexthop_entries) == (
            self._p4rt_nexthop_obj.get_original_asic_db_entries_count()
        )

        self._clean_vrf(dvs)
