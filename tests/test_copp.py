import time
import os
import pytest

from swsscommon import swsscommon


field_to_sai_attr = {
        "queue": "SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE",
        "meter_type": "SAI_POLICER_ATTR_METER_TYPE",
        "mode": "SAI_POLICER_ATTR_MODE",
        "trap_action": "SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION",
        "trap_priority": "SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY",
        "cbs": "SAI_POLICER_ATTR_CBS",
        "cir": "SAI_POLICER_ATTR_CIR",
        "red_action": "SAI_POLICER_ATTR_RED_PACKET_ACTION"
}

field_to_sai_obj_type = {
        "queue": "SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP",
        "meter_type": "SAI_OBJECT_TYPE_POLICER",
        "mode": "SAI_OBJECT_TYPE_POLICER",
        "trap_action": "SAI_OBJECT_TYPE_HOSTIF_TRAP",
        "trap_priority": "SAI_OBJECT_TYPE_HOSTIF_TRAP",
        "cbs": "SAI_OBJECT_TYPE_POLICER",
        "cir": "SAI_OBJECT_TYPE_POLICER",
        "red_action": "SAI_OBJECT_TYPE_POLICER",
        "genetlink_mcgrp_name": "SAI_OBJECT_TYPE_HOSTIF",
        "genetlink_name": "SAI_OBJECT_TYPE_HOSTIF"
}

traps_to_trap_type = {
        "stp": "SAI_HOSTIF_TRAP_TYPE_STP",
        "lacp": "SAI_HOSTIF_TRAP_TYPE_LACP",
        "eapol": "SAI_HOSTIF_TRAP_TYPE_EAPOL",
        "lldp": "SAI_HOSTIF_TRAP_TYPE_LLDP",
        "pvrst": "SAI_HOSTIF_TRAP_TYPE_PVRST",
        "igmp_query": "SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_QUERY",
        "igmp_leave": "SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_LEAVE",
        "igmp_v1_report": "SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V1_REPORT",
        "igmp_v2_report": "SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V2_REPORT",
        "igmp_v3_report": "SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V3_REPORT",
        "sample_packet": "SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET",
        "switch_cust_range": "SAI_HOSTIF_TRAP_TYPE_SWITCH_CUSTOM_RANGE_BASE",
        "arp_req": "SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST",
        "arp_resp": "SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE",
        "dhcp": "SAI_HOSTIF_TRAP_TYPE_DHCP",
        "ospf": "SAI_HOSTIF_TRAP_TYPE_OSPF",
        "pim": "SAI_HOSTIF_TRAP_TYPE_PIM",
        "vrrp": "SAI_HOSTIF_TRAP_TYPE_VRRP",
        "bgp": "SAI_HOSTIF_TRAP_TYPE_BGP",
        "dhcpv6": "SAI_HOSTIF_TRAP_TYPE_DHCPV6",
        "ospfv6": "SAI_HOSTIF_TRAP_TYPE_OSPFV6",
        "vrrpv6": "SAI_HOSTIF_TRAP_TYPE_VRRPV6",
        "bgpv6": "SAI_HOSTIF_TRAP_TYPE_BGPV6",
        "neigh_discovery": "SAI_HOSTIF_TRAP_TYPE_IPV6_NEIGHBOR_DISCOVERY",
        "mld_v1_v2": "SAI_HOSTIF_TRAP_TYPE_IPV6_MLD_V1_V2",
        "mld_v1_report": "SAI_HOSTIF_TRAP_TYPE_IPV6_MLD_V1_REPORT",
        "mld_v1_done": "SAI_HOSTIF_TRAP_TYPE_IPV6_MLD_V1_DONE",
        "mld_v2_report": "SAI_HOSTIF_TRAP_TYPE_MLD_V2_REPORT",
        "ip2me": "SAI_HOSTIF_TRAP_TYPE_IP2ME",
        "ssh": "SAI_HOSTIF_TRAP_TYPE_SSH",
        "snmp": "SAI_HOSTIF_TRAP_TYPE_SNMP",
        "router_custom_range": "SAI_HOSTIF_TRAP_TYPE_ROUTER_CUSTOM_RANGE_BASE",
        "l3_mtu_error": "SAI_HOSTIF_TRAP_TYPE_L3_MTU_ERROR",
        "ttl_error": "SAI_HOSTIF_TRAP_TYPE_TTL_ERROR",
        "udld": "SAI_HOSTIF_TRAP_TYPE_UDLD",
        "bfd": "SAI_HOSTIF_TRAP_TYPE_BFD",
        "bfdv6": "SAI_HOSTIF_TRAP_TYPE_BFDV6",
        "src_nat_miss": "SAI_HOSTIF_TRAP_TYPE_SNAT_MISS",
        "dest_nat_miss": "SAI_HOSTIF_TRAP_TYPE_DNAT_MISS"
        }

copp_group_default = {
        "queue": "0",
        "meter_type":"packets",
        "mode":"sr_tcm",
        "cir":"600",
        "cbs":"600",
        "red_action":"drop"
}

copp_group_queue4_group1 = {
        "trap_action":"trap",
        "trap_priority":"4",
        "queue": "4"
}

copp_group_queue4_group2 = {
        "trap_action":"copy",
        "trap_priority":"4",
        "queue": "4",
        "meter_type":"packets",
        "mode":"sr_tcm",
        "cir":"600",
        "cbs":"600",
        "red_action":"drop"
}

copp_group_queue4_group3 = {
        "trap_action":"trap",
        "trap_priority":"4",
        "queue": "4"
}

copp_group_queue1_group1 = {
        "trap_action":"trap",
        "trap_priority":"1",
        "queue": "1",
        "meter_type":"packets",
        "mode":"sr_tcm",
        "cir":"6000",
        "cbs":"6000",
        "red_action":"drop"
}

copp_group_queue1_group2 = {
        "trap_action":"trap",
        "trap_priority":"1",
        "queue": "1",
        "meter_type":"packets",
        "mode":"sr_tcm",
        "cir":"600",
        "cbs":"600",
        "red_action":"drop"
}

copp_group_queue2_group1 = {
	"cbs": "1000",
	"cir": "1000",
	"genetlink_mcgrp_name": "packets",
	"genetlink_name": "psample",
	"meter_type": "packets",
	"mode": "sr_tcm",
	"queue": "2",
	"red_action": "drop",
	"trap_action": "trap",
	"trap_priority": "1"
}

copp_group_queue5_group1 = {
	"cbs": "2000",
	"cir": "2000",
	"meter_type": "packets",
	"mode": "sr_tcm",
	"queue": "5",
	"red_action": "drop",
	"trap_action": "trap",
	"trap_priority": "5"
}
copp_trap = {
        "bgp,bgpv6": copp_group_queue4_group1,
        "lacp": copp_group_queue4_group1,
        "arp_req,arp_resp,neigh_discovery":copp_group_queue4_group2,
        "lldp":copp_group_queue4_group3,
        "dhcp,dhcpv6":copp_group_queue4_group3,
        "udld":copp_group_queue4_group3,
        "ip2me":copp_group_queue1_group1,
        "src_nat_miss,dest_nat_miss": copp_group_queue1_group2,
        "sample_packet": copp_group_queue2_group1,
        "ttl_error": copp_group_default
}

restricted_traps = ["sample_packet"]

policer_meter_map = {
   "packets": "SAI_METER_TYPE_PACKETS",
   "bytes": "SAI_METER_TYPE_BYTES"
}

policer_mode_map = {
    "sr_tcm": "SAI_POLICER_MODE_SR_TCM",
    "tr_tcm": "SAI_POLICER_MODE_TR_TCM",
    "storm": "SAI_POLICER_MODE_STORM_CONTROL"
}


packet_action_map = {
    "drop": "SAI_PACKET_ACTION_DROP",
    "forward": "SAI_PACKET_ACTION_FORWARD",
    "copy": "SAI_PACKET_ACTION_COPY",
    "copy_cancel": "SAI_PACKET_ACTION_COPY_CANCEL",
    "trap": "SAI_PACKET_ACTION_TRAP",
    "log": "SAI_PACKET_ACTION_LOG",
    "deny": "SAI_PACKET_ACTION_DENY",
    "transit": "SAI_PACKET_ACTION_TRANSIT"
}

class TestCopp(object):
    def setup_copp(self, dvs):
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.trap_atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP")
        self.trap_group_atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP")
        self.policer_atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        self.hostiftbl_atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF_TABLE_ENTRY")
        self.hostif_atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
        self.trap_ctbl = swsscommon.Table(self.cdb, "COPP_TRAP")
        self.trap_group_ctbl = swsscommon.Table(self.cdb, "COPP_GROUP")
        self.feature_tbl = swsscommon.Table(self.cdb, "FEATURE")
   
    def validate_policer(self, policer_oid, field, value):
        (status, fvs) = self.policer_atbl.get(policer_oid)
        assert status == True
        attr = field_to_sai_attr[field]
        
        attr_value = value
        if field == "mode":
            attr_value = policer_mode_map[value]
        elif field == "meter_type":
            attr_value = policer_meter_map[value]
        elif field == "red_action":
            attr_value = packet_action_map[value]

        for fv in fvs:
            if (fv[0] == attr):
                assert attr_value == fv[1]
                
    def validate_trap_group(self, trap_oid, trap_group):
        (status, trap_fvs) = self.trap_atbl.get(trap_oid)
        assert status == True
        trap_group_oid = ""
        policer_oid= ""
        queue = ""
        trap_action = ""
        trap_priority = ""

        for fv in trap_fvs:
            if fv[0] == "SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION":
                trap_action = fv[1]
            elif fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY":
                trap_priority = fv[1]
            elif fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP":
                trap_group_oid = fv[1]

        if trap_group_oid != "" and trap_group_oid != "oid:0x0":
            (status, fvs) = self.trap_group_atbl.get(trap_group_oid)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER":
                    policer_oid = fv[1]
                elif fv[0] == "SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE":
                    queue = fv[1]
                
        for keys in trap_group:
            obj_type = field_to_sai_obj_type[keys]
            if obj_type == "SAI_OBJECT_TYPE_POLICER":
                assert policer_oid != "" 
                assert policer_oid != "oid:0x0"
                self.validate_policer(policer_oid, keys, trap_group[keys])

            elif obj_type == "SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP":
                assert trap_group_oid != ""
                assert trap_group_oid != "oid:0x0"
                if keys == "queue":
                    assert queue == trap_group[keys]
                else:
                    assert 0

            elif obj_type == "SAI_OBJECT_TYPE_HOSTIF_TRAP":
                if keys == "trap_action":
                    assert trap_action == packet_action_map[trap_group[keys]]
                elif keys == "trap_priority":
                    assert trap_priority == trap_group[keys]

            elif obj_type == "SAI_OBJECT_TYPE_HOSTIF":
                host_tbl_keys = self.hostiftbl_atbl.getKeys();
                host_tbl_key = None
                for host_tbl_entry in host_tbl_keys:
                    (status, fvs) = self.hostiftbl_atbl.get(host_tbl_entry)
                    assert status == True
                    for fv in fvs:
                        if fv[0] == "SAI_HOSTIF_TABLE_ENTRY_ATTR_TRAP_ID":
                            if fv[1] == trap_oid:
                                host_tbl_key = host_tbl_entry
                                break
                    if host_tbl_key != None:
                        break
                assert host_tbl_key != None
                (status, fvs) = self.hostiftbl_atbl.get(host_tbl_key)
                hostif = None
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TABLE_ENTRY_ATTR_HOST_IF":
                        hostif = fv[1]
                    elif fv[0] == "SAI_HOSTIF_TABLE_ENTRY_ATTR_CHANNEL_TYPE":
                        assert fv[1] == "SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_GENETLINK"
                assert hostif != None
                (status, fvs) = self.hostif_atbl.get(hostif)
                assert status == True
                for fv in fvs:
                    if keys == "genetlink_mcgrp_name":
                        if fv[0] == "SAI_HOSTIF_ATTR_GENETLINK_MCGRP_NAME":
                            assert fv[1] == trap_group[keys]
                    if keys == "genetlink_name":
                        if fv[0] == "SAI_HOSTIF_ATTR_NAME":
                            assert fv[1] == trap_group[keys]



    def test_defaults(self, dvs, testlog):
        self.setup_copp(dvs)
        trap_keys = self.trap_atbl.getKeys()
        for traps in copp_trap:
            trap_ids = traps.split(",")
            trap_group = copp_trap[traps]
            for trap_id in trap_ids:
                trap_type = traps_to_trap_type[trap_id]
                trap_found = False
                trap_group_oid = ""
                for key in trap_keys:
                    (status, fvs) = self.trap_atbl.get(key)
                    assert status == True
                    for fv in fvs:
                        if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                           if fv[1] == trap_type:
                               trap_found = True
                    if trap_found:
                        self.validate_trap_group(key,trap_group)
                        break
                if trap_id not in restricted_traps:
                    assert trap_found == True

    def test_restricted_trap_sflow(self, dvs, testlog):
        self.setup_copp(dvs)
        fvs = swsscommon.FieldValuePairs([("status", "enabled")])
        self.feature_tbl.set("sflow", fvs)
        time.sleep(2)
        global copp_trap

        trap_keys = self.trap_atbl.getKeys()
        for traps in copp_trap:
            trap_ids = traps.split(",")
            if "sample_packet" not in trap_ids:
                continue
            trap_group = copp_trap[traps]
            trap_found = False 
            trap_type = traps_to_trap_type["sample_packet"]
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            assert trap_found == True


    def test_policer_set(self, dvs, testlog):
        self.setup_copp(dvs)
        fvs = swsscommon.FieldValuePairs([("cbs", "900")])
        self.trap_group_ctbl.set("queue4_group2", fvs)
        copp_group_queue4_group2["cbs"] = "900"
        time.sleep(2)
        global copp_trap

        trap_keys = self.trap_atbl.getKeys()
        for traps in copp_trap:
            if copp_trap[traps] != copp_group_queue4_group2:
                continue
            trap_ids = traps.split(",")
            trap_group = copp_trap[traps]
            for trap_id in trap_ids:
                trap_type = traps_to_trap_type[trap_id]
                trap_found = False
                trap_group_oid = ""
                for key in trap_keys:
                    (status, fvs) = self.trap_atbl.get(key)
                    assert status == True
                    for fv in fvs:
                        if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                           if fv[1] == trap_type:
                               trap_found = True
                    if trap_found:
                        self.validate_trap_group(key,trap_group)
                        break
                if trap_id not in restricted_traps:
                    assert trap_found == True

    def test_trap_group_set(self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        traps = "bgp,bgpv6"
        fvs = swsscommon.FieldValuePairs([("trap_group", "queue1_group1")])
        self.trap_ctbl.set("bgp", fvs)
        copp_trap[traps] = copp_group_queue1_group1
        time.sleep(2)

        trap_keys = self.trap_atbl.getKeys()
        trap_ids = traps.split(",")
        trap_group = copp_trap[traps]
        for trap_id in trap_ids:
            trap_type = traps_to_trap_type[trap_id]
            trap_found = False
            trap_group_oid = ""
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            if trap_id not in restricted_traps:
                assert trap_found == True

    def test_trap_action_set(self, dvs, testlog):
        self.setup_copp(dvs)
        fvs = swsscommon.FieldValuePairs([("trap_action", "copy")])
        self.trap_group_ctbl.set("queue4_group1", fvs)
        copp_group_queue4_group1["trap_action"] = "copy"
        time.sleep(2)
        global copp_trap

        trap_keys = self.trap_atbl.getKeys()
        for traps in copp_trap:
            if copp_trap[traps] != copp_group_queue4_group1:
                continue
            trap_ids = traps.split(",")
            trap_group = copp_trap[traps]
            for trap_id in trap_ids:
                trap_type = traps_to_trap_type[trap_id]
                trap_found = False
                trap_group_oid = ""
                for key in trap_keys:
                    (status, fvs) = self.trap_atbl.get(key)
                    assert status == True
                    for fv in fvs:
                        if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                           if fv[1] == trap_type:
                               trap_found = True
                    if trap_found:
                        self.validate_trap_group(key,trap_group)
                        break
                if trap_id not in restricted_traps:
                    assert trap_found == True

    def test_new_trap_add(self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        traps = "eapol"
        fvs = swsscommon.FieldValuePairs([("trap_group", "queue1_group2"),("trap_ids", "eapol")])
        self.trap_ctbl.set(traps, fvs)
        copp_trap[traps] = copp_group_queue1_group2
        time.sleep(2)

        trap_keys = self.trap_atbl.getKeys()
        trap_ids = traps.split(",")
        trap_group = copp_trap[traps]
        for trap_id in trap_ids:
            trap_type = traps_to_trap_type[trap_id]
            trap_found = False
            trap_group_oid = ""
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            if trap_id not in restricted_traps:
                assert trap_found == True

    def test_new_trap_del(self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        traps = "eapol"
        fvs = swsscommon.FieldValuePairs([("trap_group", "queue1_group2"),("trap_ids", "eapol")])
        self.trap_ctbl.set(traps, fvs)
        copp_trap[traps] = copp_group_queue1_group2
        time.sleep(2)

        self.trap_ctbl._del(traps)
        time.sleep(2)
        trap_ids = traps.split(",")
        trap_group = copp_trap[traps]
        trap_keys = self.trap_atbl.getKeys()
        for trap_id in trap_ids:
            trap_type = traps_to_trap_type[trap_id]
            trap_found = False
            trap_group_oid = ""
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            if trap_id not in restricted_traps:
                assert trap_found == False

    def test_new_trap_group_add(self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        traps = "igmp_v1_report"
        list_val = list(copp_group_queue5_group1.items())

        fvs = swsscommon.FieldValuePairs(list_val)
        self.trap_group_ctbl.set("queue5_group1", fvs)
        traps = "igmp_v1_report"
        t_fvs = swsscommon.FieldValuePairs([("trap_group", "queue5_group1"),("trap_ids", "igmp_v1_report")])
        self.trap_ctbl.set(traps, t_fvs)
        copp_trap[traps] = copp_group_queue5_group1
        time.sleep(2)

        trap_keys = self.trap_atbl.getKeys()
        trap_ids = traps.split(",")
        trap_group = copp_trap[traps]
        for trap_id in trap_ids:
            trap_type = traps_to_trap_type[trap_id]
            trap_found = False
            trap_group_oid = ""
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            if trap_id not in restricted_traps:
                assert trap_found == True

    def test_new_trap_group_del(self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        traps = "igmp_v1_report"
        list_val = list(copp_group_queue5_group1.items())

        fvs = swsscommon.FieldValuePairs(list_val)
        self.trap_group_ctbl.set("queue5_group1", fvs)
        traps = "igmp_v1_report"
        t_fvs = swsscommon.FieldValuePairs([("trap_group", "queue5_group1"),("trap_ids", "igmp_v1_report")])
        self.trap_ctbl.set(traps, t_fvs)
        copp_trap[traps] = copp_group_queue5_group1

        self.trap_group_ctbl._del("queue5_group1")
        time.sleep(2)

        trap_keys = self.trap_atbl.getKeys()
        trap_ids = traps.split(",")
        trap_group = copp_trap[traps]
        for trap_id in trap_ids:
            trap_type = traps_to_trap_type[trap_id]
            trap_found = False
            trap_group_oid = ""
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            if trap_id not in restricted_traps:
                assert trap_found != True

    def test_init_group_del (self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        fvs = swsscommon.FieldValuePairs([("cbs", "500"),("cir","500")])
        self.trap_group_ctbl.set("queue1_group1", fvs)
        time.sleep(2)


        self.trap_group_ctbl._del("queue1_group1")
        time.sleep(2)


        trap_keys = self.trap_atbl.getKeys()
        for traps in copp_trap:
            if copp_trap[traps] != copp_group_queue1_group1:
                continue
            trap_ids = traps.split(",")
            trap_group = copp_trap[traps]
            for trap_id in trap_ids:
                trap_type = traps_to_trap_type[trap_id]
                trap_found = False
                trap_group_oid = ""
                for key in trap_keys:
                    (status, fvs) = self.trap_atbl.get(key)
                    assert status == True
                    for fv in fvs:
                        if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                           if fv[1] == trap_type:
                               trap_found = True
                    if trap_found:
                        self.validate_trap_group(key,trap_group)
                        break
                if trap_id not in restricted_traps:
                    assert trap_found == True

    def test_init_group_del (self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        fvs = swsscommon.FieldValuePairs([("cbs", "500"),("cir","500")])
        self.trap_group_ctbl.set("queue1_group1", fvs)
        time.sleep(2)


        self.trap_group_ctbl._del("queue1_group1")
        time.sleep(2)


        trap_keys = self.trap_atbl.getKeys()
        for traps in copp_trap:
            if copp_trap[traps] != copp_group_queue1_group1:
                continue
            trap_ids = traps.split(",")
            trap_group = copp_trap[traps]
            for trap_id in trap_ids:
                trap_type = traps_to_trap_type[trap_id]
                trap_found = False
                trap_group_oid = ""
                for key in trap_keys:
                    (status, fvs) = self.trap_atbl.get(key)
                    assert status == True
                    for fv in fvs:
                        if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                           if fv[1] == trap_type:
                               trap_found = True
                    if trap_found:
                        self.validate_trap_group(key,trap_group)
                        break
                if trap_id not in restricted_traps:
                    assert trap_found == True


    def test_init_trap_del(self, dvs, testlog):
        self.setup_copp(dvs)
        global copp_trap
        traps = "ip2me,ssh"
        fvs = swsscommon.FieldValuePairs([("trap_ids", "ip2me,ssh")])
        self.trap_ctbl.set("ip2me", fvs)
        time.sleep(2)

        self.trap_ctbl._del("ip2me")
        time.sleep(2)
        trap_ids = traps.split(",")
        trap_group = copp_trap["ip2me"]
        trap_keys = self.trap_atbl.getKeys()
        for trap_id in trap_ids:
            trap_type = traps_to_trap_type[trap_id]
            trap_found = False
            trap_group_oid = ""
            for key in trap_keys:
                (status, fvs) = self.trap_atbl.get(key)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE":
                        if fv[1] == trap_type:
                            trap_found = True
                if trap_found:
                    self.validate_trap_group(key,trap_group)
                    break
            if trap_id not in restricted_traps:
                if trap_id == "ip2me":
                    assert trap_found == True
                elif trap_id == "ssh":
                    assert trap_found == False
