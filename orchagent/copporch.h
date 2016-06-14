#ifndef SWSS_COPPORCH_H
#define SWSS_COPPORCH_H

#include <map>
#include "orch.h"

const std::string copp_trap_id_list             = "trap_ids";// see valid_trap_id_list for values

const std::string copp_queue                    = "queue";

// policer values
const std::string copp_policer_meter_type       = "meter_type";
const std::string copp_policer_meter_packets    = "packets";
const std::string copp_policer_meter_bytes      = "bytes";

const std::string copp_policer_mode             = "mode";
const std::string copp_policer_mode_sr_tcm          = "sr_tcm";
const std::string copp_policer_mode_tr_tcm          = "tr_tcm";
const std::string copp_policer_mode_storm           = "storm";

const std::string copp_policer_color           = "color";
const std::string copp_policer_color_aware           = "aware";
const std::string copp_policer_color_blind           = "blind";


const std::string copp_policer_cbs              = "cbs";
const std::string copp_policer_cir              = "cir";
const std::string copp_policer_pbs              = "pbs";
const std::string copp_policer_pir              = "pir";
const std::string copp_policer_action_green     = "green_action";
const std::string copp_policer_action_red       = "red_action";
const std::string copp_policer_action_yellow    = "yellow_action";

const std::string copp_policer_action_value_drop        = "drop";
const std::string copp_policer_action_value_forward     = "forward";
const std::string copp_policer_action_value_copy        = "copy";
const std::string copp_policer_action_value_copy_cancel = "copy_cancel";
const std::string copp_policer_action_value_trap        = "trap";
const std::string copp_policer_action_value_log         = "log";
const std::string copp_policer_action_value_deny        = "deny";
const std::string copp_policer_action_value_transit     = "transit";

typedef struct _enum_to_name_t
{
    sai_hostif_trap_id_t    trap_id;
    string                  name;
} enum_to_name_t;

const vector<enum_to_name_t> valid_trap_id_list = {
    {SAI_HOSTIF_TRAP_ID_STP, "stp"},
    {SAI_HOSTIF_TRAP_ID_LACP, "lacp"},
    {SAI_HOSTIF_TRAP_ID_EAPOL, "eapol"},
    {SAI_HOSTIF_TRAP_ID_LLDP, "lldp"},
    {SAI_HOSTIF_TRAP_ID_PVRST, "pvrst"},
    {SAI_HOSTIF_TRAP_ID_IGMP_TYPE_QUERY, "igmp_query"},
    {SAI_HOSTIF_TRAP_ID_IGMP_TYPE_LEAVE, "igmp_leave"},
    {SAI_HOSTIF_TRAP_ID_IGMP_TYPE_V1_REPORT, "igmp_v1_report"},
    {SAI_HOSTIF_TRAP_ID_IGMP_TYPE_V2_REPORT, "igmp_v2_report"},
    {SAI_HOSTIF_TRAP_ID_IGMP_TYPE_V3_REPORT, "igmp_v3_report"},
    {SAI_HOSTIF_TRAP_ID_SAMPLEPACKET, "sample_packet"},
    {SAI_HOSTIF_TRAP_ID_SWITCH_CUSTOM_RANGE_BASE, "switch_cust_range"},//TODO: ?? is this needed?
    {SAI_HOSTIF_TRAP_ID_ARP_REQUEST, "arp_req"},
    {SAI_HOSTIF_TRAP_ID_ARP_RESPONSE, "arp_resp"},
    {SAI_HOSTIF_TRAP_ID_DHCP, "dhcp"},
    {SAI_HOSTIF_TRAP_ID_OSPF, "ospf"},
    {SAI_HOSTIF_TRAP_ID_PIM, "pim"},
    {SAI_HOSTIF_TRAP_ID_VRRP, "vrrp"},
    {SAI_HOSTIF_TRAP_ID_BGP, "bgp"},
    {SAI_HOSTIF_TRAP_ID_DHCPV6, "dhcpv6"},
    {SAI_HOSTIF_TRAP_ID_OSPFV6, "ospvfv6"},
    {SAI_HOSTIF_TRAP_ID_VRRPV6, "vrrpv6"},
    {SAI_HOSTIF_TRAP_ID_BGPV6, "bgpv6"},
    {SAI_HOSTIF_TRAP_ID_IPV6_NEIGHBOR_DISCOVERY, "neigh_discovery"},
    {SAI_HOSTIF_TRAP_ID_IPV6_MLD_V1_V2, "mld_v1_v2"},
    {SAI_HOSTIF_TRAP_ID_IPV6_MLD_V1_REPORT, "mld_v1_report"},
    {SAI_HOSTIF_TRAP_ID_IPV6_MLD_V1_DONE, "mld_v2_done"},
    {SAI_HOSTIF_TRAP_ID_MLD_V2_REPORT, "mld_v2_report"},
//    {SAI_HOSTIF_TRAP_ID_IP2ME, "ip2me"},
//    {SAI_HOSTIF_TRAP_ID_SSH, "ssh"},
//    {SAI_HOSTIF_TRAP_ID_SNMP, "snmp"},
    {SAI_HOSTIF_TRAP_ID_ROUTER_CUSTOM_RANGE_BASE, "router_custom_range"},//TODO: ?? is this needed?
    {SAI_HOSTIF_TRAP_ID_L3_MTU_ERROR, "l3_mtu_error"},
    {SAI_HOSTIF_TRAP_ID_TTL_ERROR, "ttl"}
};

class CoppOrch : public Orch
{
public:
    CoppOrch(DBConnector *db, string tableName);
protected:
    virtual void doTask(Consumer& consumer);
    task_process_status processCoppRule(Consumer& consumer);
    bool isValidList(vector<string> &trap_id_list, vector<string> &all_items) const;
    bool getPolicerMeter(string input, sai_meter_type_t& meter_value) const;
    bool getPolicerMode(string input, sai_policer_mode_t& mode) const;
    bool getPlicerColor(string input, sai_policer_color_source_t& color) const;
    bool getTrapID(string &trap_id_str, sai_hostif_trap_id_t &trap_id) const;
    bool getTrapIdList(vector<string> &trap_id_name_list, vector<sai_hostif_trap_id_t> &trap_id_list) const;
    bool applyTrapIds(sai_object_id_t trap_group, vector<string> &trap_id_name_list);
    bool removePolicerFromTrapGroup(sai_object_id_t trap_group);
protected:
    object_map m_trap_map;
};
#endif /* SWSS_COPPORCH_H */

