#include <assert.h>
#include <inttypes.h>
#include "arsorch.h"
#include "routeorch.h"
#include "portsorch.h"
#include "logger.h"
#include "swssnet.h"
#include <array>
#include <algorithm>
#include "sai_serialize.h"
#include "flow_counter_handler.h"

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_ars_profile_api_t*       sai_ars_profile_api;
extern sai_ars_api_t*               sai_ars_api;
extern sai_port_api_t*              sai_port_api;
extern sai_lag_api_t*               sai_lag_api;
extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;
extern sai_switch_api_t*            sai_switch_api;

extern RouteOrch *gRouteOrch;
extern PortsOrch *gPortsOrch;
extern bool gTraditionalFlexCounter;

static const map<string, sai_lag_attr_t> lag_counter_type_map = {
    {"LAG_PACKETS_DROP", SAI_LAG_ATTR_ARS_PACKET_DROPS},
    {"LAG_PORT_REASSIGNMENT", SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS}
};

static const map<string, sai_next_hop_group_attr_t> nhg_counter_type_map = {
    {"NHG_PACKETS_DROP", SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS},
    {"NHG_MEMBER_REASSIGNMENT", SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS},
    {"NHG_PORT_REASSIGNMENT", SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS}
};

static const map<sai_lag_attr_t, string> lag_counter_stat_ids = {

    {SAI_LAG_ATTR_ARS_PACKET_DROPS, "SAI_LAG_ATTR_ARS_PACKET_DROPS"},
    {SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS, "SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS"}
};

static const map<sai_next_hop_group_attr_t, string> nhg_counter_stat_ids = {

    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS, "SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS"},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS, "SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS"},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS, "SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS"}
};

ars_sai_attr_lookup_t ars_profile_attrs = {
    {SAI_ARS_PROFILE_ATTR_ALGO, {"SAI_ARS_PROFILE_ATTR_ALGO"}},
    {SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL, {"SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL"}},
    {SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED, {"SAI_ARS_PROFILE_ATTR_ARS_RANDOM_SEED"}},
    {SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS, {"SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS"}},
    {SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP, {"SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP"}},
    {SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_GROUPS, {"SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_GROUPS"}},
    {SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_MEMBERS_PER_GROUP, {"SAI_ARS_PROFILE_ATTR_LAG_ARS_MAX_MEMBERS_PER_GROUP"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_CURRENT"}},
    {SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT, {"SAI_ARS_PROFILE_ATTR_PORT_LOAD_EXPONENT"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BANDS, {"SAI_ARS_PROFILE_ATTR_QUANT_BANDS"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_0_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_1_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_2_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_3_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_4_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_5_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_6_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MIN_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MIN_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MAX_THRESHOLD, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_7_MAX_THRESHOLD"}},
    {SAI_ARS_PROFILE_ATTR_ENABLE_IPV4, {"SAI_ARS_PROFILE_ATTR_ENABLE_IPV4"}},
    {SAI_ARS_PROFILE_ATTR_ENABLE_IPV6, {"SAI_ARS_PROFILE_ATTR_ENABLE_IPV6"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_PAST, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_PAST"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_PAST, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_PAST"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_FUTURE, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_FUTURE"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_FUTURE, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_FUTURE"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL"}},
    {SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL, {"SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_CURRENT, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MIN_THRESHOLD_LIST_LOAD_CURRENT"}},
    {SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_CURRENT, {"SAI_ARS_PROFILE_ATTR_QUANT_BAND_MAX_THRESHOLD_LIST_LOAD_CURRENT"}},
    {SAI_ARS_PROFILE_ATTR_MAX_FLOWS, {"SAI_ARS_PROFILE_ATTR_MAX_FLOWS"}}
};
ars_sai_attr_lookup_t ars_sai_attrs = {
    {SAI_ARS_ATTR_MODE, {"SAI_ARS_ATTR_MODE"}},
    {SAI_ARS_ATTR_IDLE_TIME, {"SAI_ARS_ATTR_IDLE_TIME"}},
    {SAI_ARS_ATTR_MAX_FLOWS, {"SAI_ARS_ATTR_MAX_FLOWS"}},
    {SAI_ARS_ATTR_MON_ENABLE, {"SAI_ARS_ATTR_MON_ENABLE"}},
    {SAI_ARS_ATTR_SAMPLEPACKET_ENABLE, {"SAI_ARS_ATTR_SAMPLEPACKET_ENABLE"}},
    {SAI_ARS_ATTR_MAX_ALT_MEMEBERS_PER_GROUP, {"SAI_ARS_ATTR_MAX_ALT_MEMEBERS_PER_GROUP"}},
    {SAI_ARS_ATTR_MAX_PRIMARY_MEMEBERS_PER_GROUP, {"SAI_ARS_ATTR_MAX_PRIMARY_MEMEBERS_PER_GROUP"}},
    {SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD, {"SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD"}},
    {SAI_ARS_ATTR_ALTERNATE_PATH_COST, {"SAI_ARS_ATTR_ALTERNATE_PATH_COST"}},
    {SAI_ARS_ATTR_ALTERNATE_PATH_BIAS, {"SAI_ARS_ATTR_ALTERNATE_PATH_BIAS"}}
};
ars_sai_attr_lookup_t ars_port_attrs = {
    {SAI_PORT_ATTR_ARS_ENABLE, {"SAI_PORT_ATTR_ARS_ENABLE"}},
    {SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR, {"SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR"}},
    {SAI_PORT_ATTR_ARS_ALTERNATE_PATH, {"SAI_PORT_ATTR_ARS_ALTERNATE_PATH"}}
};

ars_sai_attr_lookup_t ars_nhg_attrs = {
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS"}}
};

ars_sai_attr_lookup_t ars_switch_attrs = {
    {SAI_SWITCH_ATTR_ARS_PROFILE, {"SAI_SWITCH_ATTR_ARS_PROFILE"}}
};

ars_sai_attr_lookup_t ars_lag_attrs = {
    {SAI_LAG_ATTR_ARS_OBJECT_ID, {"SAI_LAG_ATTR_ARS_OBJECT_ID"}},
    {SAI_LAG_ATTR_ARS_PACKET_DROPS, {"SAI_LAG_ATTR_ARS_PACKET_DROPS"}},
    {SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS, {"SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS"}}
};

ars_sai_feature_data_t ars_feature_switch_data =
    {"SAI_OBJECT_TYPE_SWITCH",ars_switch_attrs};

ars_sai_feature_data_t ars_feature_profile_data =
    {"SAI_OBJECT_TYPE_ARS_PROFILE",ars_profile_attrs};

ars_sai_feature_data_t ars_feature_ars_data =
    {"SAI_OBJECT_TYPE_ARS",ars_sai_attrs};

ars_sai_feature_data_t ars_feature_port_data =
    {"SAI_OBJECT_TYPE_PORT",ars_port_attrs};

ars_sai_feature_data_t ars_feature_nhg_data =
    {"SAI_OBJECT_TYPE_NEXT_HOP_GROUP",ars_nhg_attrs};

ars_sai_feature_data_t ars_feature_lag_data =
    {"SAI_OBJECT_TYPE_LAG",ars_lag_attrs};

ars_sai_feature_lookup_t ars_features =
{
    {SAI_OBJECT_TYPE_SWITCH, ars_feature_switch_data},
    {SAI_OBJECT_TYPE_ARS_PROFILE, ars_feature_profile_data},
    {SAI_OBJECT_TYPE_ARS, ars_feature_ars_data},
    {SAI_OBJECT_TYPE_PORT, ars_feature_port_data},
    {SAI_OBJECT_TYPE_NEXT_HOP_GROUP, ars_feature_nhg_data},
    {SAI_OBJECT_TYPE_LAG, ars_feature_lag_data}
};

#define ARS_FIELD_NAME_MAX_FLOWS              "max_flows"
#define ARS_FIELD_NAME_ALGORITHM              "algorithm"
#define ARS_FIELD_NAME_SAMPLE_INTERVAL        "sample_interval"
#define ARS_FIELD_NAME_PAST_LOAD_MIN_VALUE    "past_load_min_value"
#define ARS_FIELD_NAME_PAST_LOAD_MAX_VALUE    "past_load_max_value"
#define ARS_FIELD_NAME_PAST_LOAD_WEIGHT       "past_load_weight"
#define ARS_FIELD_NAME_FUTURE_LOAD_MIN_VALUE  "future_load_min_value"
#define ARS_FIELD_NAME_FUTURE_LOAD_MAX_VALUE  "future_load_max_value"
#define ARS_FIELD_NAME_FUTURE_LOAD_WEIGHT     "future_load_weight"
#define ARS_FIELD_NAME_CURRENT_LOAD_MIN_VALUE "current_load_min_value"
#define ARS_FIELD_NAME_CURRENT_LOAD_MAX_VALUE "current_load_max_value"
#define ARS_FIELD_NAME_MIN_VALUE              "min_value"
#define ARS_FIELD_NAME_MAX_VALUE              "max_value"
#define ARS_FIELD_NAME_WEIGHT                 "weight"
#define ARS_FIELD_NAME_FUTURE_LOAD            "future_load"
#define ARS_FIELD_NAME_CURRENT_LOAD           "current_load"
#define ARS_FIELD_NAME_INDEX                  "index"
#define ARS_FIELD_NAME_IPV4_ENABLE            "ipv4_enable"
#define ARS_FIELD_NAME_IPV6_ENABLE            "ipv6_enable"

#define ARS_FIELD_NAME_PROFILE_NAME "profile_name"
#define ARS_FIELD_NAME_ARS_NAME "ars_name"
#define ARS_FIELD_NAME_ASSIGN_MODE "assign_mode"
#define ARS_FIELD_NAME_PER_FLOWLET "per_flowlet_quality"
#define ARS_FIELD_NAME_PER_PACKET "per_packet"
#define ARS_FIELD_NAME_IDLE_TIME "flowlet_idle_time"
#define ARS_FIELD_NAME_QUALITY_THRESHOLD "quality_threshold"
#define ARS_FIELD_NAME_PRIMARY_PATH_THRESHOLD "primary_path_threshold"

ArsOrch::ArsOrch(DBConnector *config_db, DBConnector *appDb, DBConnector *stateDb, vector<string> &tableNames, VRFOrch *vrfOrch) :
        Orch(config_db, tableNames),
        m_vrfOrch(vrfOrch),
        m_counter_db(std::shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0))),
        m_asic_db(std::shared_ptr<DBConnector>(new DBConnector("ASIC_DB", 0))),
        m_lag_counter_table(std::unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_ARS_LAG_NAME_MAP))),
        m_nhg_counter_table(std::unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_ARS_NEXTHOP_GROUP_NAME_MAP))),
        m_vidToRidTable(std::unique_ptr<Table>(new Table(m_asic_db.get(), "VIDTORID"))),
        m_lag_counter_manager(ARS_LAG_FLEX_COUNTER_GROUP, StatsMode::READ, ARS_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, false),
        m_nhg_counter_manager(ARS_NEXTHOP_GROUP_FLEX_COUNTER_GROUP, StatsMode::READ, ARS_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, false),
        m_arsProfileStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_PROFILE_TABLE_NAME))),
        m_arsIfStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_INTERFACE_TABLE_NAME))),
        m_arsNhgStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_NEXTHOP_GROUP_TABLE_NAME))),
        m_arsLagStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_LAG_TABLE_NAME)))
{
    SWSS_LOG_ENTER();

    initCapabilities();

    if (m_isArsSupported)
    {
        gPortsOrch->attach(this);

        auto intervT = timespec { .tv_sec = 1 , .tv_nsec = 0 };

        if (isGetImplemented(SAI_OBJECT_TYPE_LAG, SAI_LAG_ATTR_ARS_PACKET_DROPS) || isGetImplemented(SAI_OBJECT_TYPE_LAG, SAI_LAG_ATTR_ARS_PORT_REASSIGNMENTS))
        {
            m_LagFlexCounterUpdTimer = new SelectableTimer(intervT);
            auto executorT = new ExecutableTimer(m_LagFlexCounterUpdTimer, this, "ARS_LAG_FLEX_COUNTER_UPD_TIMER");
            Orch::addExecutor(executorT);
        }
        if (isGetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS) || 
            isGetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS) || 
            isGetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS))
        {
            m_NhgFlexCounterUpdTimer = new SelectableTimer(intervT);
            auto executorT = new ExecutableTimer(m_NhgFlexCounterUpdTimer, this, "ARS_NHG_FLEX_COUNTER_UPD_TIMER");
            Orch::addExecutor(executorT);
        }
    }
}

void ArsOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
    assert(cntx);

    if (m_arsProfiles.empty())
    {
        SWSS_LOG_INFO("ARS not enabled - no action on interface or nexthop state change");
        return;
    }

    switch(type) {
        case SUBJECT_TYPE_PORT_OPER_STATE_CHANGE:
        {
            /* confgiure port scaling factor when port speed becomes available */
            PortOperStateUpdate *update = reinterpret_cast<PortOperStateUpdate *>(cntx);
            bool is_found = (m_arsEnabledInterfaces.find(update->port.m_alias) != m_arsEnabledInterfaces.end());
            if (is_found)
            {
                SWSS_LOG_INFO("Interface %s %senabled for ARS - %s ARS",
                        update->port.m_alias.c_str(),
                        is_found ? "" : "not ",
                        update->operStatus == SAI_PORT_OPER_STATUS_UP ? "enable" : "disable");
                if (update->operStatus == SAI_PORT_OPER_STATUS_UP)
                {
                    updateArsEnabledInterface(update->port, true);
                }
            }
            break;
        }
        case SUBJECT_TYPE_PORT_CHANGE:
        {
            /* configure port and lag ARS enable */
            bool is_lag = false;
            PortUpdate *update = reinterpret_cast<PortUpdate *>(cntx);
            if (update->port.m_lag_id && m_arsLags.find(update->port.m_alias) != m_arsLags.end())
            {
                is_lag = true;
                if (!update->add)
                {
                    /* remove from counter db */                    
                    removeLagFromFlexCounter(update->port.m_lag_id, update->port.m_alias);
                }
                break;
            }

            bool is_found = (m_arsEnabledInterfaces.find(update->port.m_alias) != m_arsEnabledInterfaces.end());
            SWSS_LOG_INFO("Interface %s is %s - %s",
                    update->port.m_alias.c_str(),
                    update->add ? "added" : "deleted",
                    is_found ? "enable ARS" : "ignore");
            if (is_found && !is_lag)
            {
                if (update->port.m_oper_status == SAI_PORT_OPER_STATUS_UP)
                {
                    updateArsEnabledInterface(update->port, true);
                }
            }

            /* check if this is lag or member of lag */
            if (is_lag)
            {
                Port lag;
                if (gPortsOrch->getPort(update->port.m_lag_id, lag))
                {
                    auto lag_object = m_arsLags.find(update->port.m_alias);
                    sai_attribute_t attr;
                    attr.id = SAI_LAG_ATTR_ARS_OBJECT_ID;

                    /* check if ars object id needs updating or should  be removed */
                    if (isSetImplemented(SAI_OBJECT_TYPE_LAG, SAI_LAG_ATTR_ARS_OBJECT_ID))
                    {
                        if (isGetImplemented(SAI_OBJECT_TYPE_LAG, SAI_LAG_ATTR_ARS_OBJECT_ID))
                        {

                            sai_status_t status = sai_lag_api->get_lag_attribute(update->port.m_lag_id, 1, &attr);
                            if (status != SAI_STATUS_SUCCESS)
                            {
                                SWSS_LOG_ERROR("Failed to get ars id of lag: %s", update->port.m_alias.c_str());
                            }
                        }

                        if (attr.value.oid != lag_object->second.ars_object_id)
                        {
                            attr.value.oid = lag_object->second.ars_object_id;
                            sai_status_t status = sai_lag_api->set_lag_attribute(update->port.m_lag_id, &attr);
                            if (status != SAI_STATUS_SUCCESS)
                            {
                                SWSS_LOG_ERROR("Failed to set poid %" PRIx64 " to lag: %s", attr.value.oid, update->port.m_alias.c_str());
                            }
                            addLagToFlexCounter(update->port.m_lag_id, update->port.m_alias);
                        }
                    }
                }
            }
            break;
        }
        case SUBJECT_TYPE_NEXTHOP_CHANGE:
        {
            NextHopUpdate *update = reinterpret_cast<NextHopUpdate *>(cntx);
            /* verify that it is a ARS NHG */
            std::string vrf_name;
            if (update->vrf_id == gVirtualRouterId)
            {
                vrf_name = "default";
            }
            else
            {
                vrf_name = m_vrfOrch->getVRFname(update->vrf_id);
            }
            if (vrf_name == "")
            {
                break;
            }
            auto nhg_table = m_arsNexthopGroupPrefixes.find(vrf_name);
            if (nhg_table == m_arsNexthopGroupPrefixes.end())
            {
                break;
            }
            auto ars_object = nhg_table->second.find(update->prefix);
            if (ars_object == nhg_table->second.end())
            {
                break;
            }
            /* don't rely on reported nhg - use cached info */
            auto nhg_sai_id = gRouteOrch->getNextHopGroupId(update->nexthopGroup);
            if (nhg_sai_id == SAI_NULL_OBJECT_ID)
            {
                break;
            }
            /* if NHG is not yet configured */
            if (update->nexthopGroup.getSize() > 1 && ars_object->second.nexthops.find(update->nexthopGroup) == ars_object->second.nexthops.end())
            {
                if (!isSetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID))
                {
                    SWSS_LOG_NOTICE("Remove existing NHG and create ARS-enabled");
                    if (gRouteOrch->reconfigureNexthopGroupWithArsState(update->nexthopGroup, nhg_sai_id, ars_object->second.ars_object_id))
                    {
                        addNhgToFlexCounter(nhg_sai_id, update->prefix, vrf_name);
                    }
                }
                /* just update the NHG attr */
                else if (gRouteOrch->updateNexthopGroupArsState(nhg_sai_id, ars_object->second.ars_object_id))
                {
                    addNhgToFlexCounter(nhg_sai_id, update->prefix, vrf_name);
                }
                ars_object->second.nexthops.insert(update->nexthopGroup);
            }
            break;
        }
        default:
            break;
    }
}

bool ArsOrch::bake()
{
    SWSS_LOG_ENTER();

    if (!m_isArsSupported)
    {
        SWSS_LOG_NOTICE("ARS not supported - no action");
        return true;
    }

    SWSS_LOG_NOTICE("Warm reboot: placeholder");

    return Orch::bake();
}

bool ArsOrch::createArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;
    vector<sai_attribute_t> supported_ars_attrs;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (isCreateImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, a.id))
        {
            supported_ars_attrs.push_back(a);
            SWSS_LOG_NOTICE("ARS profile %s. Setting Attr %d value %u",
                             profile.profile_name.c_str(), a.id, a.value.u32);
        }
        else
        {
            if (a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s value %s",
                               a.id, profile.profile_name.c_str(), a.value.booldata ? "true" : "false");
            }
            else
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s value %u",
                               a.id, profile.profile_name.c_str(), a.value.u32);
            }
            continue;
        }
    }

    if (supported_ars_attrs.empty())
    {
        SWSS_LOG_WARN("No supported attributes found for ARS profile %s", profile.profile_name.c_str());
        return false;
    }

    status = sai_ars_profile_api->create_ars_profile(&profile.m_sai_ars_id,
                                                     gSwitchId,
                                                     (uint32_t)supported_ars_attrs.size(),
                                                     supported_ars_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ars profile %s: %d", profile.profile_name.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS_PROFILE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Created ARS profile %s (oid %" PRIx64 ")", profile.profile_name.c_str(), profile.m_sai_ars_id);

    return true;
}

bool ArsOrch::setArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (!isSetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, a.id))
        {
            if (a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || a.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s (oid %" PRIx64 ") value %s",
                               a.id, profile.profile_name.c_str(), profile.m_sai_ars_id, a.value.booldata ? "true" : "false");
            }
            else
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s (oid %" PRIx64 ") value %u",
                               a.id, profile.profile_name.c_str(), profile.m_sai_ars_id, a.value.u32);
            }
            continue;
        }

        status = sai_ars_profile_api->set_ars_profile_attribute(profile.m_sai_ars_id, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ars profile %s (oid %" PRIx64 ") attr %d: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, a.id, status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    return true;
}

bool ArsOrch::createArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;
    vector<sai_attribute_t> supported_ars_attrs;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (isCreateImplemented(SAI_OBJECT_TYPE_ARS, a.id))
        {
            supported_ars_attrs.push_back(a);
            SWSS_LOG_NOTICE("ARS %s. Setting Attr %d value %u",
                             object->profile_name.c_str(), a.id, a.value.u32);
        }
        else
        {
            SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS %s value %u",
                           a.id, object->profile_name.c_str(), a.value.u32);
            continue;
        }
    }

    if (supported_ars_attrs.empty())
    {
        SWSS_LOG_WARN("No supported attributes found for ARS %s", object->profile_name.c_str());
        return false;
    }

    status = sai_ars_api->create_ars(&object->ars_object_id,
                                     gSwitchId,
                                     (uint32_t)supported_ars_attrs.size(),
                                     supported_ars_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ars %s: %d", object->profile_name.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool ArsOrch::setArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;

    /* go over set of attr and set only supported attributes  */
    for (auto a : ars_attrs)
    {
        if (!isSetImplemented(SAI_OBJECT_TYPE_ARS, a.id))
        {
            SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS %s (oid %" PRIx64 ") value %u",
                        a.id, object->profile_name.c_str(), object->ars_object_id, a.value.u32);
            continue;
        }

        status = sai_ars_api->set_ars_attribute(object->ars_object_id, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ars %s (oid %" PRIx64 ") attr %d: %d",
                            object->profile_name.c_str(), object->ars_object_id, a.id, status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    return true;
}

bool ArsOrch::delArsObject(ArsObjectEntry *object)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;

    status = sai_ars_api->remove_ars(object->ars_object_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ars %s (oid %" PRIx64 ": %d)",
                        object->profile_name.c_str(), object->ars_object_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool ArsOrch::updateArsEnabledInterface(const Port &port, const bool is_enable)
{
    SWSS_LOG_ENTER();

    if (isSetImplemented(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_ARS_ENABLE))
    {
    if (!gPortsOrch->setPortArsEnable(port, is_enable))
        {
            SWSS_LOG_ERROR("Failed to set ars enable for port %s", port.m_alias.c_str());
            return false;
        }
    }

    if (isSetImplemented(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR))
    {
        if (is_enable && !gPortsOrch->setPortArsLoadScaling(port))
        {
            SWSS_LOG_ERROR("Failed to set ars load scaling factor for port %s", port.m_alias.c_str());
            return false;
        }
    }

    SWSS_LOG_NOTICE("Interface %s - %sable ARS on interface",
                    port.m_alias.c_str(),
                    is_enable ? "en" : "dis");

    return true;
}

bool ArsOrch::doTaskArsProfile(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
         string table_id = key.substr(0, found);
        string ars_profile_name = key.substr(found + 1);
        string op = kfvOp(t);
    
        uint32_t max_flows = 0, sampling_interval = 0, past_load_min_val = 0, past_load_max_val = 0, past_load_weight = 0;
        uint32_t future_load_min_val = 0, future_load_max_val = 0, future_load_weight = 0, current_load_min_val = 0, current_load_max_val = 0;
        bool ipv6_enable = false, ipv4_enable = false;
        ArsAlgorithm algo = ARS_ALGORITHM_EWMA;

        bool is_new_entry = false;
        sai_attribute_t         ars_attr;
        vector<sai_attribute_t> ars_attrs;

        SWSS_LOG_NOTICE("OP: %s, Profile: %s", op.c_str(), ars_profile_name.c_str());

        auto arsProfile_entry = m_arsProfiles.find(ars_profile_name);

        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple name("profile_name", ars_profile_name);
            fvVector.push_back(name);

            if (arsProfile_entry != m_arsProfiles.end())
            {
                max_flows = arsProfile_entry->second.max_flows;
                algo = arsProfile_entry->second.algorithm;
                sampling_interval = arsProfile_entry->second.sampling_interval;
                ipv4_enable = arsProfile_entry->second.ipv4_enabled;
                ipv6_enable = arsProfile_entry->second.ipv6_enabled;
                past_load_min_val = arsProfile_entry->second.path_metrics.past_load.min_value;
                past_load_max_val = arsProfile_entry->second.path_metrics.past_load.max_value;
                past_load_weight = arsProfile_entry->second.path_metrics.past_load.weight;
                future_load_min_val = arsProfile_entry->second.path_metrics.future_load.min_value;
                future_load_max_val = arsProfile_entry->second.path_metrics.future_load.max_value;
                future_load_weight = arsProfile_entry->second.path_metrics.future_load.weight;
                current_load_min_val = arsProfile_entry->second.path_metrics.future_load.min_value;
                current_load_max_val = arsProfile_entry->second.path_metrics.future_load.max_value;
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_MAX_FLOWS)
                {
                    max_flows = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_MAX_FLOWS;
                    ars_attr.value.u32 = max_flows;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_ALGORITHM)
                {
                    algo = (ArsAlgorithm)stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_ALGO;
                    ars_attr.value.u32 = algo;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_SAMPLE_INTERVAL)
                {
                    sampling_interval = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_SAMPLING_INTERVAL;
                    ars_attr.value.u32 = sampling_interval;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_IPV4_ENABLE)
                {
                    ipv4_enable = (fvValue(i) == "true");
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV4;
                    ars_attr.value.booldata = ipv4_enable;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_IPV6_ENABLE)
                {
                    ipv6_enable = (fvValue(i) == "true");
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_ENABLE_IPV6;
                    ars_attr.value.booldata = ipv6_enable;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_PAST_LOAD_MIN_VALUE)
                {
                    past_load_min_val = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MIN_VAL;
                    ars_attr.value.u32 = past_load_min_val;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_PAST_LOAD_MAX_VALUE)
                {
                    past_load_max_val = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_PAST_MAX_VAL;
                    ars_attr.value.u32 = past_load_max_val;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_PAST_LOAD_WEIGHT)
                {
                    past_load_weight = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_PAST_WEIGHT;
                    ars_attr.value.u32 = past_load_weight;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_FUTURE_LOAD_MIN_VALUE)
                {
                    future_load_min_val = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MIN_VAL;
                    ars_attr.value.u32 = future_load_min_val;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_FUTURE_LOAD_MAX_VALUE)
                {
                    future_load_max_val = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_FUTURE_MAX_VAL;
                    ars_attr.value.u32 = future_load_max_val;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_FUTURE_LOAD_WEIGHT)
                {
                    future_load_weight = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_PORT_LOAD_FUTURE_WEIGHT;
                    ars_attr.value.u32 = future_load_weight;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_CURRENT_LOAD_MIN_VALUE)
                {
                    current_load_min_val = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MIN_VAL;
                    ars_attr.value.u32 = current_load_min_val;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_CURRENT_LOAD_MAX_VALUE)
                {
                    current_load_max_val = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_PROFILE_ATTR_LOAD_CURRENT_MAX_VAL;
                    ars_attr.value.u32 = current_load_max_val;
                    ars_attrs.push_back(ars_attr);
                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    break;
                }
                FieldValueTuple value(fvField(i), fvValue(i));
                fvVector.push_back(value);
            }

            if (arsProfile_entry == m_arsProfiles.end())
            {
                ArsProfileEntry arsProfileEntry;
                arsProfileEntry.profile_name = ars_profile_name;
                m_arsProfiles[ars_profile_name] = arsProfileEntry;
                arsProfile_entry = m_arsProfiles.find(ars_profile_name);
                SWSS_LOG_NOTICE("Added new ARS profile %s", ars_profile_name.c_str());
                is_new_entry = true;
            }

            arsProfile_entry->second.max_flows = max_flows;
            arsProfile_entry->second.algorithm = algo;
            arsProfile_entry->second.sampling_interval = sampling_interval;
            arsProfile_entry->second.ipv4_enabled = ipv4_enable;
            arsProfile_entry->second.ipv6_enabled = ipv6_enable;
            arsProfile_entry->second.path_metrics.past_load.min_value = past_load_min_val;
            arsProfile_entry->second.path_metrics.past_load.max_value = past_load_max_val;
            arsProfile_entry->second.path_metrics.past_load.weight = past_load_weight;
            arsProfile_entry->second.path_metrics.future_load.min_value = future_load_min_val;
            arsProfile_entry->second.path_metrics.future_load.max_value = future_load_max_val;
            arsProfile_entry->second.path_metrics.future_load.weight = future_load_weight;
            arsProfile_entry->second.path_metrics.current_load.min_value = current_load_min_val;
            arsProfile_entry->second.path_metrics.current_load.max_value = current_load_max_val;

            bool res;
            if (is_new_entry)
            {
                res = createArsProfile(arsProfile_entry->second, ars_attrs);
            }
            else
            {
                res = setArsProfile(arsProfile_entry->second, ars_attrs);
            }

            if (!res)
            {
                SWSS_LOG_ERROR("Failed to create/set ARS profile %s", ars_profile_name.c_str());
                continue;
            }

            m_arsProfileStateTable->set(ars_profile_name, fvVector);
        }
        else if (op == DEL_COMMAND)
        {
            if (arsProfile_entry == m_arsProfiles.end())
            {
                SWSS_LOG_NOTICE("Received delete call for non-existent entry %s", ars_profile_name.c_str());
            }
            else 
            {
                /* Check if there are no child objects associated prior to deleting */
                if (arsProfile_entry->second.ref_count == 0 && m_arsEnabledInterfaces.empty())
                {
                    SWSS_LOG_INFO("Received delete call for valid entry with no further dependencies, deleting %s",
                        ars_profile_name.c_str());
                }
                else
                {
                    SWSS_LOG_NOTICE("Child Prefix/Member entries are still associated with this ARS profile %s", 
                            ars_profile_name.c_str());
                    continue;
                }

                m_arsProfiles.erase(arsProfile_entry);
                m_arsProfileStateTable->del(ars_profile_name);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
    return true;
}

bool ArsOrch::doTaskArsInterfaces(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (m_arsProfiles.empty())
    {
        SWSS_LOG_WARN("No ARS profiles exist");
        return false;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string table_id = key.substr(0, found);
        string if_name = key.substr(found + 1);
        string op = kfvOp(t);
        vector<sai_attribute_t> ars_attrs;
        Port p;

        SWSS_LOG_NOTICE("ARS Path Op %s Interface %s", op.c_str(), if_name.c_str());

        if (op == SET_COMMAND)
        {
            if (m_arsEnabledInterfaces.find(if_name) != m_arsEnabledInterfaces.end()) 
            {
                SWSS_LOG_WARN("Tried to add already added interface %s - skipped", if_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
        
            m_arsEnabledInterfaces.insert(if_name);
            SWSS_LOG_NOTICE("Added new ARS-enabled interface %s", if_name.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsEnabledInterfaces.find(if_name) == m_arsEnabledInterfaces.end())
            {
                SWSS_LOG_INFO("Received delete call for non-existent interface %s", if_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else
            {
                m_arsEnabledInterfaces.erase(if_name);
                SWSS_LOG_INFO("Removed interface %s", if_name.c_str());
            }
        }

        if (!gPortsOrch->getPort(if_name, p) || p.m_port_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Tried to %s non-existent/down interface %s - skipped", op.c_str(), if_name.c_str());
        }
        else
        {
            updateArsEnabledInterface(p, (op == SET_COMMAND));
        }

        it = consumer.m_toSync.erase(it);
    }

    string ifnames = "";
    for (auto ifname : m_arsEnabledInterfaces)
    {
        ifnames += ifname + " ";
    }
    FieldValueTuple tmp("ifname", ifnames);
    vector<FieldValueTuple> fvVector;
    fvVector.push_back(tmp);
    m_arsIfStateTable->set("", fvVector);

    return true;
}

bool ArsOrch::doTaskArsNexthopGroup(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        IpPrefix ip_prefix = IpPrefix(key.substr(0, found));
        string vrf_name = key.substr(found + 1);
        string op = kfvOp(t);
        bool is_new_entry = false;
        sai_attribute_t         ars_attr;
        vector<sai_attribute_t> ars_attrs;
        string ars_profile_name = "";

        SWSS_LOG_NOTICE("ARS Prefix Op %s, Profile: %s, Vrf %s, Prefix %s", op.c_str(), ars_profile_name.c_str(), vrf_name.c_str(), ip_prefix.to_string().c_str());

        auto table = m_arsNexthopGroupPrefixes.find(vrf_name);
        sai_object_id_t vrf_id = m_vrfOrch->getVRFid(vrf_name);
        NextHopGroupKey nhg = gRouteOrch->getSyncdRouteNhgKey(vrf_id, ip_prefix);

        ArsObjectEntry *arsObject_entry = nullptr;
        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvVector;

            if (table == m_arsNexthopGroupPrefixes.end())
            {
                is_new_entry = true;
                ArsNexthopGroupPrefixes tmp_nhg_table;
                ArsObjectEntry arsObjectEntry;
                arsObjectEntry.profile_name = "dummy";
                tmp_nhg_table[ip_prefix] = arsObjectEntry;
                m_arsNexthopGroupPrefixes[vrf_name] = tmp_nhg_table;
                table = m_arsNexthopGroupPrefixes.find(vrf_name);
            }

            auto& nhg_table = table->second;

            if (nhg_table.find(ip_prefix) == nhg_table.end())
            {
                is_new_entry = true;
                ArsObjectEntry arsObjectEntry;
                arsObjectEntry.profile_name = "dummy";
                nhg_table[ip_prefix] = arsObjectEntry;
            }

            arsObject_entry = &nhg_table[ip_prefix];

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_PROFILE_NAME)
                {
                    ars_profile_name = fvValue(i);
                    arsObject_entry->profile_name = ars_profile_name;
                }
                else if (fvField(i) == ARS_FIELD_NAME_MAX_FLOWS)
                {
                    arsObject_entry->max_flows = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_MAX_FLOWS;
                    ars_attr.value.u32 = arsObject_entry->max_flows;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_IDLE_TIME)
                {
                    arsObject_entry->flowlet_idle_time = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_IDLE_TIME;
                    ars_attr.value.u32 = arsObject_entry->flowlet_idle_time;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_ASSIGN_MODE)
                {
                    arsObject_entry->assign_mode = PER_FLOWLET_QUALITY;
                    ars_attr.id = SAI_ARS_ATTR_MODE;
                    ars_attr.value.u32 = SAI_ARS_MODE_FLOWLET_QUALITY;
                    if (fvValue(i) == ARS_FIELD_NAME_PER_PACKET)
                    {
                        arsObject_entry->assign_mode = PER_PACKET;
                        ars_attr.value.u32 = SAI_ARS_MODE_PER_PACKET_QUALITY;
                    }
                    else if (fvValue(i) != ARS_FIELD_NAME_PER_FLOWLET)
                    {
                        SWSS_LOG_WARN("Received unsupported assign_mode %s, defaulted to per_flowlet_quality",
                                        fvValue(i).c_str());
                        break;
                    }
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_PRIMARY_PATH_THRESHOLD)
                {
                    arsObject_entry->quality_threshold.primary_threshold = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD;
                    ars_attr.value.u32 = arsObject_entry->quality_threshold.primary_threshold;
                    ars_attrs.push_back(ars_attr);
                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    continue;
                }

                FieldValueTuple value(fvField(i), fvValue(i));
                fvVector.push_back(value);
            }

            if (ars_profile_name.empty())
            {
                SWSS_LOG_WARN("Received ARS NHG Vrf %s, Prefix %s without profile name", vrf_name.c_str(), ip_prefix.to_string().c_str());
                continue;
            }

            auto arsProfile_entry = m_arsProfiles.find(ars_profile_name);
            if (arsProfile_entry == m_arsProfiles.end())
            {
                SWSS_LOG_WARN("Received ARS NHG Vrf %s, Prefix %s for non-existent profile %s", vrf_name.c_str(), ip_prefix.to_string().c_str(), ars_profile_name.c_str());
                continue;
            }

            if (is_new_entry)
            {
                arsObject_entry->profile_name = ars_profile_name;
            }

            FieldValueTuple tmp("nexthops", nhg.to_string());
            fvVector.push_back(tmp);

            if (is_new_entry)
            {
                createArsObject(arsObject_entry, ars_attrs);
                gRouteOrch->attach(this, ip_prefix.getIp(), vrf_id);
                arsProfile_entry->second.ref_count++;
            }
            else
            {
                setArsObject(arsObject_entry, ars_attrs);
            }

            SWSS_LOG_NOTICE("Ars entry added for profile %s, prefix %s, NHs %s", ars_profile_name.c_str(), ip_prefix.to_string().c_str(), nhg.to_string().c_str());

            m_arsNhgStateTable->set(vrf_name + "_" + ip_prefix.to_string(), fvVector);

            if (nhg.getSize() > 0)
            {
                /* Handling ARS over already configured nexthop groups */
                /* Enable ARS over NHG */
                auto nhg_sai_id = gRouteOrch->getNextHopGroupId(nhg);
                if (nhg_sai_id)
                {
                    if (!isSetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID))
                    {
                        SWSS_LOG_NOTICE("Remove existing NHG and create ARS-enabled");
                        if (gRouteOrch->reconfigureNexthopGroupWithArsState(nhg, nhg_sai_id, arsObject_entry->ars_object_id))
                        {
                            addNhgToFlexCounter(nhg_sai_id, ip_prefix, vrf_name);
                        }
                    }
                    /* just update the NHG attr */
                    else if (gRouteOrch->updateNexthopGroupArsState(nhg_sai_id, arsObject_entry->ars_object_id))
                    {
                        addNhgToFlexCounter(nhg_sai_id, ip_prefix, vrf_name);
                    }
                    arsObject_entry->nexthops.insert(nhg);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            auto table = m_arsNexthopGroupPrefixes.find(vrf_name);
            if (table == m_arsNexthopGroupPrefixes.end() || table->second.find(ip_prefix) == table->second.end())
            {
                SWSS_LOG_NOTICE("ARS_NHG_PREFIX doesn't exists, ignore");
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else if (!isSetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID))
            {
                SWSS_LOG_NOTICE("Disabling ARS NHG is not supported");
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else
            {
                arsObject_entry = &table->second[ip_prefix];
                if (arsObject_entry->ars_object_id)
                {
                    /* handle existing nexthop group */
                    if (nhg.getSize() > 0)
                    {
                        /* Disable ARS over NHG */
                        auto sai_id = gRouteOrch->getNextHopGroupId(nhg);
                        if (sai_id)
                        {
                            removeNhgFromFlexCounter(sai_id, ip_prefix, vrf_name);
                            gRouteOrch->updateNexthopGroupArsState(sai_id, SAI_NULL_OBJECT_ID);
                            arsObject_entry->nexthops.erase(nhg);
                        }
                    }
                    gRouteOrch->detach(this, ip_prefix.getIp(), vrf_id);

                    /* remove ars sai object */
                    delArsObject(arsObject_entry);
                }

                table->second.erase(ip_prefix);
                if (table->second.empty())
                {
                    m_arsNexthopGroupPrefixes.erase(vrf_name);
                }
                SWSS_LOG_NOTICE("Ars entry removed for profile %s, vrf_name %s,prefix %s", ars_profile_name.c_str(), vrf_name.c_str(), ip_prefix.to_string().c_str());

                m_arsNhgStateTable->del(vrf_name + "_"  + ip_prefix.to_string());

                auto arsProfile_entry = m_arsProfiles.find(arsObject_entry->profile_name);
                if (arsProfile_entry == m_arsProfiles.end())
                {
                    SWSS_LOG_ERROR("Received ARS NHG Vrf %s, Prefix %s for non-existent profile %s", vrf_name.c_str(), ip_prefix.to_string().c_str(), arsObject_entry->profile_name.c_str());
                }
                else
                {
                    arsProfile_entry->second.ref_count--;
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }

    return true;
}


bool ArsOrch::doTaskArsLag(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string if_name = key.substr(0, found);
        string op = kfvOp(t);
        bool is_new_entry = false;
        sai_attribute_t         ars_attr;
        vector<sai_attribute_t> ars_attrs;
        Port p;
        string ars_profile_name = "";
        ArsObjectEntry *arsObject_entry = nullptr;

        if (op == SET_COMMAND)
        {
            if (m_arsLags.find(if_name) == m_arsLags.end())
            {
                is_new_entry = true;
                ArsObjectEntry arsObjectEntry;
                arsObjectEntry.profile_name = "dummy";
                m_arsLags[if_name] = arsObjectEntry;
            }

            arsObject_entry = &m_arsLags[if_name];
            SWSS_LOG_NOTICE("ARS Lag Op %s, Profile: %s, name %s", op.c_str(), arsObject_entry->profile_name.c_str(), if_name.c_str());

            vector<FieldValueTuple> fvVector;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_PROFILE_NAME)
                {
                    ars_profile_name = fvValue(i);
                    arsObject_entry->profile_name = ars_profile_name;
                }
                else if (fvField(i) == ARS_FIELD_NAME_MAX_FLOWS)
                {
                    arsObject_entry->max_flows = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_MAX_FLOWS;
                    ars_attr.value.u32 = arsObject_entry->max_flows;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_IDLE_TIME)
                {
                    arsObject_entry->flowlet_idle_time = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_IDLE_TIME;
                    ars_attr.value.u32 = arsObject_entry->flowlet_idle_time;
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_ASSIGN_MODE)
                {
                    arsObject_entry->assign_mode = PER_FLOWLET_QUALITY;
                    ars_attr.id = SAI_ARS_ATTR_MODE;
                    ars_attr.value.u32 = SAI_ARS_MODE_FLOWLET_QUALITY;
                    if (fvValue(i) == ARS_FIELD_NAME_PER_PACKET)
                    {
                        arsObject_entry->assign_mode = PER_PACKET;
                        ars_attr.value.u32 = SAI_ARS_MODE_PER_PACKET_QUALITY;
                    }
                    else if (fvValue(i) != ARS_FIELD_NAME_PER_FLOWLET)
                    {
                        SWSS_LOG_WARN("Received unsupported assign_mode %s, defaulted to per_flowlet_quality",
                                        fvValue(i).c_str());
                        break;
                    }
                    ars_attrs.push_back(ars_attr);
                }
                else if (fvField(i) == ARS_FIELD_NAME_PRIMARY_PATH_THRESHOLD)
                {
                    arsObject_entry->quality_threshold.primary_threshold = stoi(fvValue(i));
                    ars_attr.id = SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD;
                    ars_attr.value.u32 = arsObject_entry->quality_threshold.primary_threshold;
                    ars_attrs.push_back(ars_attr);
                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    continue;
                }

                FieldValueTuple value(fvField(i), fvValue(i));
                fvVector.push_back(value);
            }

            if (ars_profile_name.empty())
            {
                SWSS_LOG_WARN("Received ARS LAG %s without profile name", if_name.c_str());
                continue;
            }

            auto arsProfile_entry = m_arsProfiles.find(ars_profile_name);

            if (arsProfile_entry == m_arsProfiles.end())
            {
                SWSS_LOG_WARN("Received ARS Lag name %s for non-existent profile %s", if_name.c_str(), ars_profile_name.c_str());
                continue;
            }

            if (is_new_entry)
            {
                arsObject_entry->profile_name = ars_profile_name;
            }

            arsObject_entry->lags.insert(if_name);

            if (is_new_entry)
            {
                createArsObject(arsObject_entry, ars_attrs);
                arsProfile_entry->second.ref_count++;
            }
            else
            {
                setArsObject(arsObject_entry, ars_attrs);
            }

            m_arsLags[if_name] = *arsObject_entry;

            m_arsLagStateTable->set(if_name, fvVector);

            SWSS_LOG_NOTICE("ARS Adding LAG %s", if_name.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            if (m_arsLags.find(if_name) == m_arsLags.end())
            {
                SWSS_LOG_INFO("Received delete call for non-existent interface %s", if_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else if (!isSetImplemented(SAI_OBJECT_TYPE_LAG, SAI_LAG_ATTR_ARS_OBJECT_ID))
            {
                SWSS_LOG_NOTICE("Disabling ARS LAG is not supported");
                it = consumer.m_toSync.erase(it);
                continue;
            }
            else
            {
                arsObject_entry = &m_arsLags[if_name];
                if (arsObject_entry->ars_object_id)
                {
                    /* remove ars sai object */
                    delArsObject(arsObject_entry);
                }
                m_arsLags.erase(if_name);
                auto arsProfile_entry = m_arsProfiles.find(arsObject_entry->profile_name);
                if (arsProfile_entry == m_arsProfiles.end())
                {
                    SWSS_LOG_ERROR("Received ARS LAG %s for non-existent profile %s", if_name.c_str(), arsObject_entry->profile_name.c_str());
                }
                else
                {
                    arsProfile_entry->second.ref_count--;
                }
                m_arsLagStateTable->del(if_name);
                SWSS_LOG_INFO("Removed interface %s", if_name.c_str());
            }
        }

        if (!gPortsOrch->getPort(if_name, p) || (p.m_lag_id == SAI_NULL_OBJECT_ID))
        {
            SWSS_LOG_WARN("Tried to %s non-existent interface %s - skipped", op.c_str(), if_name.c_str());
        }
        else
        {
            sai_attribute_t attr;
            attr.id = SAI_LAG_ATTR_ARS_OBJECT_ID;
            attr.value.oid = (op == SET_COMMAND)  ? arsObject_entry->ars_object_id : SAI_NULL_OBJECT_ID;
            sai_status_t status = sai_lag_api->set_lag_attribute(p.m_lag_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set ars object %" PRIx64 " to lag: %s", attr.value.oid, p.m_alias.c_str());
            }
            else 
            {
                addLagToFlexCounter(p.m_lag_id, p.m_alias);
            }
        }

        it = consumer.m_toSync.erase(it);
    }

    return true;
}

void ArsOrch::doTask(Consumer& consumer) 
{
    SWSS_LOG_ENTER();
    const string & table_name = consumer.getTableName();

    if (!m_isArsSupported)
    {
        SWSS_LOG_WARN("ARS is not supported");
        return;
    }

	if (table_name == CFG_ARS_PROFILE || table_name == APP_ARS_PROFILE_TABLE_NAME)
    {
        doTaskArsProfile(consumer);
    }
    else if (table_name == CFG_ARS_INTERFACE || table_name == APP_ARS_INTERFACE_TABLE_NAME)
    {
        doTaskArsInterfaces(consumer);
    }
    else if (table_name == CFG_ARS_NEXTHOP_GROUP || table_name == APP_ARS_NEXTHOP_GROUP_TABLE_NAME)
    {
        doTaskArsNexthopGroup(consumer);
    }
    else if (table_name == CFG_ARS_PORTCHANNEL || table_name == APP_ARS_PORTCHANNEL_TABLE_NAME)
    {
        doTaskArsLag(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
    }
}
#if 0
string ArsOrch::getLagFlexCounterTableKey(string key)
{
    return string(ARS_LAG_FLEX_COUNTER_GROUP) + ":" + key;
}

string ArsOrch::getNhgFlexCounterTableKey(string key)
{
    return string(ARS_NEXTHOP_GROUP_FLEX_COUNTER_GROUP) + ":" + key;
}
#endif
std::unordered_set<std::string> ArsOrch::generateLagCounterStats()
{
    std::unordered_set<std::string> counter_stats;
    for (const auto& it: lag_counter_stat_ids)
    {
        if (isGetImplemented(SAI_OBJECT_TYPE_LAG, it.first))
        {
            counter_stats.emplace(it.second);
        }
    }
    return counter_stats;
}

std::unordered_set<std::string> ArsOrch::generateNhgCounterStats()
{
    std::unordered_set<std::string> counter_stats;
    for (const auto& it: nhg_counter_stat_ids)
    {
        if (isGetImplemented(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, it.first))
        {
            counter_stats.emplace(it.second);
        }
    }
    return counter_stats;
}

void ArsOrch::generateLagCounterMap()
{
    if (m_isLagCounterMapGenerated)
    {
        return;
    }

    if (m_LagFlexCounterUpdTimer)
    {
        m_LagFlexCounterUpdTimer->start();
        m_isLagCounterMapGenerated = true;
    }
}

void ArsOrch::generateNexthopGroupCounterMap()
{
    if (m_isNhgCounterMapGenerated)
    {
        return;
    }

    if (m_NhgFlexCounterUpdTimer)
    {
        m_NhgFlexCounterUpdTimer->start();
        m_isNhgCounterMapGenerated = true;
    }
}

void ArsOrch::addLagToFlexCounter(sai_object_id_t oid, const string &name)
{
    m_pendingLagAddToFlexCntr[oid] = name;
}

void ArsOrch::addNhgToFlexCounter(sai_object_id_t oid, const IpPrefix &prefix, const string &vrf)
{
    m_pendingNhgAddToFlexCntr[oid] = prefix.to_string() + ":" + vrf;
}

void ArsOrch::removeLagFromFlexCounter(sai_object_id_t id, const string &name)
{
    SWSS_LOG_ENTER();

    std::string counter_oid_str;
    counter_oid_str = sai_serialize_object_id(id);

    auto update_iter = m_pendingLagAddToFlexCntr.find(id);
    if (update_iter == m_pendingLagAddToFlexCntr.end())
    {
        m_lag_counter_manager.clearCounterIdList(id);
    }
    else
    {
        m_pendingLagAddToFlexCntr.erase(update_iter);
    }

    /* remove it from COUNTERS_DB maps */
    m_lag_counter_table->hdel("", name);
    m_vidToRidTable->hdel("", counter_oid_str);

    SWSS_LOG_DEBUG("Unregistered interface %s from Flex counter", name.c_str());
}

void ArsOrch::removeNhgFromFlexCounter(sai_object_id_t id, const IpPrefix &prefix, const string &vrf)
{
    SWSS_LOG_ENTER();

    std::string counter_oid_str;
    counter_oid_str = sai_serialize_object_id(id);

    auto update_iter = m_pendingNhgAddToFlexCntr.find(id);
    if (update_iter == m_pendingNhgAddToFlexCntr.end())
    {
        m_nhg_counter_manager.clearCounterIdList(id);
    }
    else
    {
        m_pendingNhgAddToFlexCntr.erase(update_iter);
    }

    /* remove it from COUNTERS_DB maps */
    auto name = prefix.to_string() + ":" + vrf;
    m_nhg_counter_table->hdel("", name);
    m_vidToRidTable->hdel("", counter_oid_str);

    SWSS_LOG_DEBUG("Unregistered nhg %s from Flex counter", name.c_str());
}

void ArsOrch::doTask(SelectableTimer &timer)
{
    string value;

    if (!m_isArsSupported)
    {
        SWSS_LOG_INFO("ARS is not supported");
        return;
    }

    if (timer.getFd() == m_LagFlexCounterUpdTimer->getFd())
    {
        for (auto it = m_pendingLagAddToFlexCntr.begin(); it != m_pendingLagAddToFlexCntr.end(); )
        {
            const auto id = sai_serialize_object_id(it->first);
            if (!gTraditionalFlexCounter || m_vidToRidTable->hget("", id, value))
            {
                vector<FieldValueTuple> lagNameVector;

                lagNameVector.emplace_back(it->second.c_str(), id);
                m_lag_counter_table->set("", lagNameVector);
                m_counter_db->hset(COUNTERS_ARS_LAG_NAME_MAP, it->second.c_str(), id.c_str());

                auto lag_counter_stats = generateLagCounterStats();
                m_lag_counter_manager.setCounterIdList(it->first, CounterType::ARS_LAG, lag_counter_stats, gSwitchId);
                SWSS_LOG_DEBUG("inserted %s to flex counter", it->second.c_str());
                it = m_pendingLagAddToFlexCntr.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (timer.getFd() == m_NhgFlexCounterUpdTimer->getFd())
    {
        for (auto it = m_pendingNhgAddToFlexCntr.begin(); it != m_pendingNhgAddToFlexCntr.end(); )
        {
            const auto id = sai_serialize_object_id(it->first);
            if (!gTraditionalFlexCounter || m_vidToRidTable->hget("", id, value))
            {
                vector<FieldValueTuple> nhgNameVector;

                nhgNameVector.emplace_back(it->second.c_str(), id);
                m_nhg_counter_table->set("", nhgNameVector);
                m_counter_db->hset(COUNTERS_ARS_NEXTHOP_GROUP_NAME_MAP, it->second.c_str(), id.c_str());

                auto nhg_counter_stats = generateNhgCounterStats();
                m_nhg_counter_manager.setCounterIdList(it->first, CounterType::ARS_NEXTHOP_GROUP, nhg_counter_stats, gSwitchId);
                SWSS_LOG_DEBUG("inserted %s to flex counter", it->second.c_str());
                it = m_pendingNhgAddToFlexCntr.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}