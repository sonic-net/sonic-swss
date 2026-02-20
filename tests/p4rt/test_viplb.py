from swsscommon import swsscommon

import pytest
import json
import util
import time
import l3
import viplb
import tables_definition


def getCrmCounterValue(dvs, key, counter):

    counters_db = swsscommon.DBConnector(
        swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    crm_stats_table = swsscommon.Table(counters_db, 'CRM')

    for k in crm_stats_table.get(key)[1]:
        if k[0] == counter:
            return int(k[1])

    return 0


def crm_update(dvs, field, value):
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(cfg_db, "CRM")
    fvs = swsscommon.FieldValuePairs([(field, value)])
    tbl.set("Config", fvs)
    time.sleep(1)


class TestP4RTVIPLB(object):

    def _set_up(self, dvs):
        self._p4rt_tables_definition_obj = tables_definition.P4RtTableDefinitionWrapper()
        self._p4rt_router_intf_obj = l3.P4RtRouterInterfaceWrapper()
        self._p4rt_neighbor_obj = l3.P4RtNeighborWrapper()
        self._p4rt_nexthop_obj = l3.P4RtNextHopWrapper()
        self._p4rt_viplb_obj = viplb.P4RtVIPLBWrapper()

        self._p4rt_tables_definition_obj.set_up_databases(dvs)
        self._p4rt_router_intf_obj.set_up_databases(dvs)
        self._p4rt_neighbor_obj.set_up_databases(dvs)
        self._p4rt_nexthop_obj.set_up_databases(dvs)
        self._p4rt_viplb_obj.set_up_databases(dvs)

    def test_VIPv4LBWithGoodNexthopAddUpdateDeletePass(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        crm_update(dvs, "polling_interval", "1")

        # Create tables definition AppDb entry
        tables_definition_key, attr_list = (
            self._p4rt_tables_definition_obj.create_tables_definition()
        )
        self._p4rt_tables_definition_obj.verify_response(tables_definition_key,
                                                         attr_list, "SWSS_RC_SUCCESS")

        # Set IP type for viplb object.
        self._p4rt_viplb_obj.set_ip_type("IPV4")

        # Maintain list of original Application and ASIC DB entries before
        # adding new entry.
        db_list = ((self._p4rt_nexthop_obj.asic_db,
                    self._p4rt_nexthop_obj.ASIC_DB_TBL_NAME),)
        self._p4rt_nexthop_obj.get_original_redis_entries(db_list)
        db_list = ((self._p4rt_viplb_obj.appl_db,
                    "%s:%s" % (self._p4rt_viplb_obj.APP_DB_TBL_NAME,
                               self._p4rt_viplb_obj.TBL_NAME)),
                   (self._p4rt_viplb_obj.asic_db,
                    self._p4rt_viplb_obj.ASIC_DB_TBL_NAME))
        self._p4rt_viplb_obj.get_original_redis_entries(db_list)

        # Create router interface.
        router_interface_id, router_intf_key, attr_list = (
            self._p4rt_router_intf_obj.create_router_interface()
        )
        self._p4rt_router_intf_obj.verify_response(router_intf_key,
                                                   attr_list, "SWSS_RC_SUCCESS")

        # Create neighbor.
        neighbor_id, neighbor_key, attr_list = (
            self._p4rt_neighbor_obj.create_neighbor()
        )
        self._p4rt_neighbor_obj.verify_response(neighbor_key, attr_list,
                                                "SWSS_RC_SUCCESS")

        # Create nexthop.
        first_nexthop_id, first_nexthop_key, attr_list = (
            self._p4rt_nexthop_obj.create_next_hop()
        )
        self._p4rt_nexthop_obj.verify_response(first_nexthop_key, attr_list,
                                               "SWSS_RC_SUCCESS")
        # get nexthop_oid of newly created nexthop
        first_nexthop_oid = self._p4rt_nexthop_obj.get_newly_created_nexthop_oid()
        assert first_nexthop_oid is not None

        # Create viplb.
        viplb_key, attr_list = (
            self._p4rt_viplb_obj.create_viplb(first_nexthop_id)
        )
        self._p4rt_viplb_obj.verify_response(viplb_key, attr_list,
                                             "SWSS_RC_SUCCESS")

        # Query application database for viplb entries.
        viplb_entries = util.get_keys(
            self._p4rt_viplb_obj.appl_db,
            self._p4rt_viplb_obj.APP_DB_TBL_NAME + ":" + self._p4rt_viplb_obj.TBL_NAME)
        assert len(viplb_entries) == (
            self._p4rt_viplb_obj.get_original_appl_db_entries_count() + 1
        )

        # Query application database for newly created viplb key.
        (status, fvs) = util.get_key(self._p4rt_viplb_obj.appl_db,
                                     self._p4rt_viplb_obj.APP_DB_TBL_NAME,
                                     viplb_key)
        assert status == True
        util.verify_attr(fvs, attr_list)

        # get programmable_object_oid of newly created viplb
        viplb_oid = self._p4rt_viplb_obj.get_newly_created_programmable_object_oid()
        assert viplb_oid is not None

        # get crm counters
        time.sleep(1)
        used_counter = getCrmCounterValue(
            dvs, "EXT_TABLE_STATS:"+self._p4rt_viplb_obj.TBL_NAME, 'crm_stats_extension_table_used')
        avail_counter = getCrmCounterValue(
            dvs, "EXT_TABLE_STATS:"+self._p4rt_viplb_obj.TBL_NAME, 'crm_stats_extension_table_available')
        assert used_counter is 1

        # Create another router interface.
        router_interface_id, router_intf_key, attr_list = (
            self._p4rt_router_intf_obj.create_router_interface(
                router_interace_id="20")
        )
        self._p4rt_router_intf_obj.verify_response(router_intf_key,
                                                   attr_list, "SWSS_RC_SUCCESS")

        # Create another neighbor.
        neighbor_id, neighbor_key, attr_list = (
            self._p4rt_neighbor_obj.create_neighbor(
                router_interface_id="20", neighbor_id="10.0.0.1")
        )
        self._p4rt_neighbor_obj.verify_response(neighbor_key, attr_list,
                                                "SWSS_RC_SUCCESS")

        # Create another nexthop.
        second_nexthop_id, second_nexthop_key, attr_list = (
            self._p4rt_nexthop_obj.create_next_hop(
                router_interface_id="20", neighbor_id="10.0.0.1", nexthop_id="16")
        )
        self._p4rt_nexthop_obj.verify_response(second_nexthop_key, attr_list,
                                               "SWSS_RC_SUCCESS")

        # Update viplb.
        viplb_key, attr_list = (
            self._p4rt_viplb_obj.create_viplb(second_nexthop_id)
        )
        self._p4rt_viplb_obj.verify_response(viplb_key, attr_list,
                                             "SWSS_RC_SUCCESS")

        # Remove nexthop.
        self._p4rt_nexthop_obj.remove_app_db_entry(first_nexthop_key)
        self._p4rt_nexthop_obj.verify_response(first_nexthop_key, [],
                                               "SWSS_RC_SUCCESS")

        # get crm counters
        time.sleep(1)
        used_counter = getCrmCounterValue(
            dvs, "EXT_TABLE_STATS:"+self._p4rt_viplb_obj.TBL_NAME, 'crm_stats_extension_table_used')
        avail_counter = getCrmCounterValue(
            dvs, "EXT_TABLE_STATS:"+self._p4rt_viplb_obj.TBL_NAME, 'crm_stats_extension_table_available')
        assert used_counter is 1

        # Remove viplb entry.
        self._p4rt_viplb_obj.remove_app_db_entry(viplb_key)
        self._p4rt_viplb_obj.verify_response(
            viplb_key, [], "SWSS_RC_SUCCESS")

        # get crm counters
        time.sleep(1)
        used_counter = getCrmCounterValue(
            dvs, "EXT_TABLE_STATS:"+self._p4rt_viplb_obj.TBL_NAME, 'crm_stats_extension_table_used')
        avail_counter = getCrmCounterValue(
            dvs, "EXT_TABLE_STATS:"+self._p4rt_viplb_obj.TBL_NAME, 'crm_stats_extension_table_available')
        assert used_counter is 0

    def test_VIPv4LBWithBadNexthopAddUpdateDeletePass(self, dvs, testlog):
        # Initialize L3 objects and database connectors.
        self._set_up(dvs)
        return

        # Create tables definition AppDb entry
        tables_definition_key, attr_list = (
            self._p4rt_tables_definition_obj.create_tables_definition()
        )
        self._p4rt_tables_definition_obj.verify_response(tables_definition_key,
                                                         attr_list, "SWSS_RC_SUCCESS")

        # Set IP type for viplb object.
        self._p4rt_viplb_obj.set_ip_type("IPV4")

        # Create viplb.
        viplb_key, attr_list = (
            self._p4rt_viplb_obj.create_viplb()
        )
        self._p4rt_viplb_obj.verify_response(viplb_key, attr_list,
                                             "SWSS_RC_INVALID_PARAM", "[OrchAgent] Cross-table reference valdiation failed, no OID found")
