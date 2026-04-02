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
extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_switch_api_t*            sai_switch_api;

extern PortsOrch *gPortsOrch;

static ars_sai_attr_lookup_t ars_profile_attrs = {
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
static ars_sai_attr_lookup_t ars_obj_attrs = {
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
static ars_sai_attr_lookup_t ars_port_attrs = {
    {SAI_PORT_ATTR_ARS_ENABLE, {"SAI_PORT_ATTR_ARS_ENABLE"}},
    {SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR, {"SAI_PORT_ATTR_ARS_PORT_LOAD_SCALING_FACTOR"}},
    {SAI_PORT_ATTR_ARS_ALTERNATE_PATH, {"SAI_PORT_ATTR_ARS_ALTERNATE_PATH"}}
};

static ars_sai_attr_lookup_t ars_nhg_attrs = {
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_PACKET_DROPS"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_NEXT_HOP_REASSIGNMENTS"}},
    {SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS, {"SAI_NEXT_HOP_GROUP_ATTR_ARS_PORT_REASSIGNMENTS"}}
};

static ars_sai_attr_lookup_t ars_switch_attrs = {
    {SAI_SWITCH_ATTR_ARS_PROFILE, {"SAI_SWITCH_ATTR_ARS_PROFILE"}}
};

static ars_sai_feature_data_t ars_feature_switch_data =
    {"SAI_OBJECT_TYPE_SWITCH",ars_switch_attrs};

static ars_sai_feature_data_t ars_feature_profile_data =
    {"SAI_OBJECT_TYPE_ARS_PROFILE",ars_profile_attrs};

static ars_sai_feature_data_t ars_feature_obj_data =
    {"SAI_OBJECT_TYPE_ARS",ars_obj_attrs};

static ars_sai_feature_data_t ars_feature_port_data =
    {"SAI_OBJECT_TYPE_PORT",ars_port_attrs};

static ars_sai_feature_data_t ars_feature_nhg_data =
    {"SAI_OBJECT_TYPE_NEXT_HOP_GROUP",ars_nhg_attrs};

static ars_sai_feature_lookup_t ars_features =
{
    {SAI_OBJECT_TYPE_SWITCH, ars_feature_switch_data},
    {SAI_OBJECT_TYPE_ARS_PROFILE, ars_feature_profile_data},
    {SAI_OBJECT_TYPE_ARS, ars_feature_obj_data},
    {SAI_OBJECT_TYPE_PORT, ars_feature_port_data},
    {SAI_OBJECT_TYPE_NEXT_HOP_GROUP, ars_feature_nhg_data},
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

#define ARS_FIELD_NAME_PROFILE_NAME           "profile_name"
#define ARS_FIELD_NAME_ARS_OBJ_NAME           "ars_obj_name"
#define ARS_FIELD_NAME_ASSIGN_MODE            "assign_mode"
#define ARS_FIELD_NAME_PER_FLOWLET            "per_flowlet_quality"
#define ARS_FIELD_NAME_PER_PACKET             "per_packet_quality"
#define ARS_FIELD_NAME_IDLE_TIME              "flowlet_idle_time"
#define ARS_FIELD_NAME_OBJ_MAX_FLOWS          "max_flows"
#define ARS_FIELD_NAME_QUALITY_THRESHOLD      "quality_threshold"
#define ARS_FIELD_NAME_SCALING_FACTOR         "scaling_factor"
#define ARS_FIELD_NAME_PRIMARY_PATH_THRESHOLD  "primary_path_threshold"
#define ARS_FIELD_NAME_ALTERNATIVE_PATH_COST   "alternative_path_cost"


#define ARS_FIELD_NAME_NHG_PATH_SELECTOR_MODE  "ars_nhg_path_selector_mode"
#define ARS_FIELD_NAME_LAG_PATH_SELECTOR_MODE  "ars_lag_path_selector_mode"
#define ARS_FIELD_NAME_DEFAULT_OBJECT          "default_ars_object"
#define ARS_FIELD_NAME_OBJECT_NAME             "ars_obj_name"
#define ARS_FIELD_NAME_ROLE                    "role"

ArsOrch::ArsOrch(DBConnector *config_db, DBConnector *appDb, DBConnector *stateDb, vector<string> &tableNames, VRFOrch *vrfOrch) :
        Orch(config_db, tableNames),
        m_vrfOrch(vrfOrch),
        m_arsProfileStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_PROFILE_TABLE_NAME))),
        m_arsCapabilityStateTable(std::unique_ptr<Table>(new Table(stateDb, STATE_ARS_CAPABILITY_TABLE_NAME)))
{
    SWSS_LOG_ENTER();

    initCapabilities();

    if (m_isArsSupported)
    {
        gPortsOrch->attach(this);
    }
}


bool ArsOrch::isSetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
{
    auto feature = ars_features.find((uint32_t)object_type);
    if (feature == ars_features.end())
    {
        return false;
    }
    auto attr = feature->second.attrs.find(attr_id);
    if (attr == feature->second.attrs.end())
    {
        return false;
    }
    return attr->second.set_implemented;
}

bool ArsOrch::isCreateImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
{
    auto feature = ars_features.find((uint32_t)object_type);
    if (feature == ars_features.end())
    {
        return false;
    }
    auto attr = feature->second.attrs.find(attr_id);
    if (attr == feature->second.attrs.end())
    {
        return false;
    }
    return attr->second.create_implemented;
}

bool ArsOrch::isGetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
{
    auto feature = ars_features.find((uint32_t)object_type);
    if (feature == ars_features.end())
    {
        return false;
    }
    auto attr = feature->second.attrs.find(attr_id);
    if (attr == feature->second.attrs.end())
    {
        return false;
    }
    return attr->second.get_implemented;
}

void ArsOrch::initCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t capability = {};

    for (auto it = ars_features.begin(); it != ars_features.end(); it++)
    {
        for (auto it2 = it->second.attrs.begin(); it2 != it->second.attrs.end(); it2++)
        {
            if (sai_query_attribute_capability(gSwitchId, (sai_object_type_t)it->first,
                                                    (sai_attr_id_t)it2->first,
                                                    &capability) == SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_NOTICE("Feature %s Attr %s is supported. Create %s Set %s Get %s", it->second.name.c_str(), it2->second.attr_name.c_str(), capability.create_implemented ? "Y" : "N", capability.set_implemented ? "Y" : "N", capability.get_implemented ? "Y" : "N");
            }
            else
            {
                SWSS_LOG_NOTICE("Feature %s Attr %s is NOT supported", it->second.name.c_str(), it2->second.attr_name.c_str());
            }

            it2->second.create_implemented = capability.create_implemented;
            it2->second.set_implemented = capability.set_implemented;
            it2->second.get_implemented = capability.get_implemented;

            vector<FieldValueTuple> fieldValues;
            fieldValues.emplace_back("create", capability.create_implemented ? "true" : "false");
            fieldValues.emplace_back("set", capability.set_implemented ? "true" : "false");
            fieldValues.emplace_back("get", capability.get_implemented ? "true" : "false");
            m_arsCapabilityStateTable->set(it->second.name + "|" + it2->second.attr_name, fieldValues);
        }
    }

    m_isArsSupported = isCreateImplemented(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ARS_PROFILE);
}

void ArsOrch::deinitCapabilities()
{
    SWSS_LOG_ENTER();

    // Clear out ARS capability state entries
    if (m_arsCapabilityStateTable)
    {
        for (auto &feature : ars_features)
        {
            for (auto &attr : feature.second.attrs)
            {
                std::string key = feature.second.name + "|" + attr.second.attr_name;
                m_arsCapabilityStateTable->del(key);
            }
        }
    }

    // Reset internal flags and structures
    m_isArsSupported = false;

    SWSS_LOG_NOTICE("ArsOrch capabilities deinitialized");
}

void ArsOrch::deinit()
{
    SWSS_LOG_ENTER();

    if (m_isArsSupported && gPortsOrch)
    {
        gPortsOrch->detach(this);
    }

    deinitCapabilities();

    SWSS_LOG_NOTICE("ArsOrch deinitialized");
}

void ArsOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
    assert(cntx);

    if (m_arsProfiles.empty())
    {
        SWSS_LOG_INFO("ARS not enabled - no action on interface state change");
        return;
    }

    switch(type) {
        case SUBJECT_TYPE_PORT_OPER_STATE_CHANGE:
        {
            /* configure port scaling factor when port speed becomes available */
            PortOperStateUpdate *update = reinterpret_cast<PortOperStateUpdate *>(cntx);
            SWSS_LOG_NOTICE("ARS port notification - port %s state %s", update->port.m_alias.c_str(), update->operStatus == SAI_PORT_OPER_STATUS_UP ? "enable" : "disable");
            auto ars_if = m_arsEnabledInterfaces.find(update->port.m_alias);
            bool is_found = (ars_if != m_arsEnabledInterfaces.end());
            if (is_found)
            {
                SWSS_LOG_INFO("Interface %s %senabled for ARS - %s ARS",
                        update->port.m_alias.c_str(),
                        is_found ? "" : "not ",
                        update->operStatus == SAI_PORT_OPER_STATUS_UP ? "enable" : "disable");
                if (update->operStatus == SAI_PORT_OPER_STATUS_UP)
                {
                    updateArsEnabledInterface(update->port, ars_if->second.first, true);
                }
            }
            break;
        }
        default:
            break;
    }
}

bool ArsOrch::validateNexthopsForArs(sai_object_id_t vrf_id, const NextHopGroupKey &nextHops, sai_object_id_t &ars_obj_id) 
{
    SWSS_LOG_ENTER();

    std::string common_ars_obj;
    std::string common_ars_obj_nh;
    std::string ars_obj_nh;
    bool is_ars_capable_nhg = false;
    ArsSelectorModeNhg selector_mode = getNhgSelectorMode();
    if (selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INVALID)
    {
        SWSS_LOG_DEBUG("Invalid ARS NHG selector mode — cannot validate nexthops");
        return false;
    }

    // --------------------------------------------------
    // 1. GLOBAL MODE
    // --------------------------------------------------
    if (selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_GLOBAL)
    {
        if (!isDefaultArsObjectValid(selector_mode, common_ars_obj))
        {
            SWSS_LOG_DEBUG("Default ars object is not configured for GLOBAL mode");
            return false;
        }
        /* Need to fill sai object */
        is_ars_capable_nhg = true;
    }

    auto &nhk_map = nextHops.getNextHops();
    if (nhk_map.empty())
    {
        SWSS_LOG_DEBUG("No nexthops found for VRF %lu in ARS validation", vrf_id);
        return false;
    }

    // --------------------------------------------------
    // 2. INTERFACE MODE
    // --------------------------------------------------
    if (selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE)
    {
        bool allArsCapable = validateInterfaceModeArs(nextHops.getNextHops(), selector_mode, common_ars_obj);
        if (allArsCapable)
        {
            SWSS_LOG_NOTICE("All INTERFACE-mode nexthop interfaces are ARS capable");
            is_ars_capable_nhg = true;
        }
        else
        {
            SWSS_LOG_NOTICE("Some INTERFACE-mode nexthop interfaces are NOT ARS capable");
            return false;
        }
    }

    // --------------------------------------------------
    // 3. NEXTHOP MODE
    // --------------------------------------------------
    if (selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_NEXTHOP)
    {
        for (const auto &nh: nhk_map)
        {
            std::string nexthop_ip = nh.ip_address.to_string();
            std::string alias = nh.alias; /* interface name (EthernetX, PortChannelX, etc.) */
            std::string vrf_name;
            vrf_name = m_vrfOrch->getVRFname(vrf_id); 
            auto nexthop_key = std::make_pair(vrf_name, nexthop_ip);
            auto nexthopIt = m_arsNexthops.find(nexthop_key);
            if (nexthopIt == m_arsNexthops.end())
            {
                SWSS_LOG_NOTICE("ARS nexthop entry not found for VRF %s and nexthop %s",
                    vrf_name.c_str(), nexthop_ip.c_str());
                return false;
            }
            const ArsNexthopEntry &ars_entry = nexthopIt->second;
            ars_obj_nh = ars_entry.ars_obj_name;
            if (common_ars_obj_nh.empty())
            {
                common_ars_obj_nh = ars_obj_nh;
            }
            else if (ars_obj_nh != common_ars_obj_nh)
            {
                SWSS_LOG_NOTICE("Nexthop %s has ARS object %s, differs from others (%s)",
                             nexthop_ip.c_str(), ars_obj_nh.c_str(), common_ars_obj_nh.c_str());
                return false;
            }
        }
        bool allArsCapable = validateInterfaceModeArs(nextHops.getNextHops(), selector_mode, common_ars_obj);
        if (allArsCapable)
        {
            SWSS_LOG_NOTICE("All nextop-mode nexthop interfaces are ARS capable");
            common_ars_obj = ars_obj_nh;
            is_ars_capable_nhg = true;
        }
    }
    if (is_ars_capable_nhg)
    {
        ars_obj_id = getArsObjectId(common_ars_obj);
        return true;
    }
    return false;
}

bool ArsOrch::validatePortOrSubPorts(const std::string &alias, std::string &ars_object)
{
    SWSS_LOG_ENTER();
    Port port;
    if (!gPortsOrch->getPort(alias, port))
    {
        SWSS_LOG_NOTICE("Port %s not found", alias.c_str());
        return false;
    }

    switch (port.m_type)
    {
        case Port::LAG:
            if (port.m_members.empty())
            {
                SWSS_LOG_NOTICE("PortChannel %s has no members", alias.c_str());
                return false;
            }
            for (const auto &member : port.m_members)
            {
                if (!validatePortOrSubPorts(member, ars_object))
                    return false;
            }
            return true;

        default:
            /* Physical port or sub-interface */
            std::string checkAlias = alias;
            auto dotPos = alias.find('.');
            if (dotPos != std::string::npos)
                checkAlias = alias.substr(0, dotPos);

            if (!isPortArsCapable(checkAlias, ars_object))
            {
                SWSS_LOG_NOTICE("Port %s is not ARS capable", checkAlias.c_str());
                return false;
            }
            return true;
    }
}

bool ArsOrch::validateInterfaceModeArs(const std::set<NextHopKey> &nextHops, ArsSelectorModeNhg selector_mode, std::string &common_ars_obj)
{
    SWSS_LOG_ENTER();

    std::set<std::string> nh_member_list;
    bool first_iface = true;
    std::string ars_obj;
    Port port;
    // Step 1: collect unique interfaces from nexthops
    for (const auto &nh : nextHops)
    {
        if (!nh.alias.empty())
            nh_member_list.insert(nh.alias); //nhMemberList
    }

    // Step 2: validate each interface
    for (const auto &alias : nh_member_list)
    {
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_NOTICE("Interface %s not found", alias.c_str());
            return false;
        }

        switch (port.m_type)
        {
            case Port::VLAN:
                if (port.m_members.empty())
                {
                    SWSS_LOG_NOTICE("VLAN %s has no member ports", alias.c_str());
                    return false;
                }
                for (const auto &member : port.m_members)
                {
                    if (!validatePortOrSubPorts(member, ars_obj))
                    {
                        SWSS_LOG_NOTICE("Underlying port %s under VLAN %s is not ARS capable",
                                        member.c_str(), alias.c_str());
                        return false;
                    }
                    // Ensure ARS object consistency across all interfaces
                    
                    if (selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE)
                    {
                        if (first_iface)
                        {
                            common_ars_obj = ars_obj;
                            first_iface = false;
                        }
                        else if (ars_obj != common_ars_obj)
                        {
                            SWSS_LOG_NOTICE("Interface %s has ARS object %s (expected %s)", 
                                alias.c_str(), ars_obj.c_str(), common_ars_obj.c_str());
                            return false;
                        }
                    }
                }
                break;

            default:
                /* Physical port, sub-interface, or PortChannel */
                if (!validatePortOrSubPorts(alias, ars_obj)) {
                    SWSS_LOG_NOTICE("ARS is not capable for interface '%s' with ARS object '%s'",
                       alias.c_str(), ars_obj.c_str());
                    return false;
                 }
                /* Ensure ARS object consistency across all interfaces */
                if (selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE)
                {
                    if (first_iface)
                    {
                            common_ars_obj = ars_obj;
                            first_iface = false;
                    }
                   else if (ars_obj != common_ars_obj)
                   {
                       SWSS_LOG_NOTICE("Interface %s has ARS object %s (expected %s)", 
                           alias.c_str(), ars_obj.c_str(), common_ars_obj.c_str());
                       return false;
                   }
                }
                break;
        }
        default:
            break;
    }

    SWSS_LOG_DEBUG("All interfaces are ARS capable");
    return true;
}

ArsSelectorModeNhg ArsOrch::getNhgSelectorMode() const
{
    SWSS_LOG_ENTER();
    if (m_arsProfiles.empty())
    {
        SWSS_LOG_NOTICE("No ARS profile found in m_arsProfiles");
        return ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INVALID;
    }

    const ArsProfileEntry& profile = m_arsProfiles.begin()->second;

    // Sanity check in case mode is invalid or uninitialized
    if (profile.nhg_selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INVALID)
    {
        SWSS_LOG_NOTICE("ARS profile has invalid NHG selector mode");
        return ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INVALID;
    }

    return profile.nhg_selector_mode;
}

bool ArsOrch::isDefaultArsObjectValid(ArsSelectorModeNhg selector_mode, std::string &default_ars_object) const
{
    SWSS_LOG_ENTER();

    /* Only GLOBAL mode requires a default ARS object */
    if (selector_mode != ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_GLOBAL)
    {
        return false;
    }

    if (m_arsProfiles.empty())
    {
        SWSS_LOG_ERROR("No ARS profile available");
        return false;
    }

    const ArsProfileEntry &profile = m_arsProfiles.begin()->second;

    if (profile.default_ars_object.empty())
    {
        SWSS_LOG_ERROR("Default ARS object is missing for GLOBAL mode");
        return false;
    }

    default_ars_object = profile.default_ars_object;

    SWSS_LOG_DEBUG("Default ARS object validated for GLOBAL mode: %s",
                   default_ars_object.c_str());
    return true;
}


bool ArsOrch::isPortArsCapable(const std::string &if_name, std::string &ars_object)
{
    SWSS_LOG_ENTER();
    auto it = m_arsEnabledInterfaces.find(if_name);
    if (it == m_arsEnabledInterfaces.end())
    {
        SWSS_LOG_DEBUG("Interface %s is not ARS enabled", if_name.c_str());
        return false;
    }

    ars_object = it->second.second; // ARS object
    SWSS_LOG_DEBUG("Interface %s is ARS enabled with ARS object %s", if_name.c_str(), ars_object.c_str());
    return true;
}

sai_object_id_t ArsOrch::getArsObjectId(const std::string &ars_obj_name) const
{
    SWSS_LOG_ENTER();
    auto it = m_arsObjects.find(ars_obj_name);
    if (it == m_arsObjects.end())
    {
        SWSS_LOG_NOTICE("ARS object %s not found", ars_obj_name.c_str());
        return SAI_NULL_OBJECT_ID;
    }

    return it->second.ars_object_id;
}

bool ArsOrch::bake()
{
    SWSS_LOG_ENTER();

    if (!m_isArsSupported)
    {
        SWSS_LOG_NOTICE("ARS not supported - no action");
        return true;
    }

    return Orch::bake();
}


bool ArsOrch::isArsProfileEnabled () const
{
    SWSS_LOG_ENTER();

    if (!m_arsProfiles.empty())
    {
        SWSS_LOG_INFO("ARS profiles exist.");
        return true;
    }

    SWSS_LOG_INFO("No ARS profiles configured in m_arsProfiles");
    return false;
}

bool ArsOrch::createArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t    status = SAI_STATUS_NOT_SUPPORTED;
    vector<sai_attribute_t> supported_ars_attrs;

    /* go over set of attr and set only supported attributes  */
    for (auto attr : ars_attrs)
    {
        if (isCreateImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, attr.id))
        {
            supported_ars_attrs.push_back(attr);
            SWSS_LOG_NOTICE("ARS profile %s. Setting Attr %d value %u",
                             profile.profile_name.c_str(), attr.id, attr.value.u32);
        }
        else
        {
            if (attr.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || attr.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s value %s",
                               attr.id, profile.profile_name.c_str(), attr.value.booldata ? "true" : "false");
            }
            else
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s value %u",
                               attr.id, profile.profile_name.c_str(), attr.value.u32);
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

    // update read-only attributes
    sai_attribute_t attr;
    if (isGetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS))
    {
        attr.id = SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS;
        status = sai_ars_profile_api->get_ars_profile_attribute(profile.m_sai_ars_id, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get ars profile %s (oid 0x%" PRIx64 ") attr SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_GROUPS: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, status);
            attr.value.u32 = 0;
        }
    }
    profile.max_ecmp_groups = attr.value.u32;

    if (isGetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP))
    {
        attr.id = SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP;
        status = sai_ars_profile_api->get_ars_profile_attribute(profile.m_sai_ars_id, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get ars profile %s (oid 0x%" PRIx64 ") attr SAI_ARS_PROFILE_ATTR_ECMP_ARS_MAX_MEMBERS_PER_GROUP: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, status);
            attr.value.u32 = 0;
        }
    }
    profile.max_ecmp_members_per_group = attr.value.u32;

    sai_attribute_t switch_attr;
    switch_attr.id = SAI_SWITCH_ATTR_ARS_PROFILE;
    switch_attr.value.oid = profile.m_sai_ars_id;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &switch_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set switch ARS profile to %s: %d",
                       profile.profile_name.c_str(), status);
    }
    else
    {
        SWSS_LOG_NOTICE("Set switch ARS profile to %s (oid 0x%" PRIx64 ")",
                        profile.profile_name.c_str(), profile.m_sai_ars_id);
    }

    SWSS_LOG_NOTICE("Created ARS profile %s (oid 0x%" PRIx64 ")", profile.profile_name.c_str(), profile.m_sai_ars_id);

    return true;
}

bool ArsOrch::setArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_NOT_SUPPORTED;

    /* go over set of attr and set only supported attributes  */
    for (auto attr : ars_attrs)
    {
        if (!isSetImplemented(SAI_OBJECT_TYPE_ARS_PROFILE, attr.id))
        {
            if (attr.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV4 || attr.id == SAI_ARS_PROFILE_ATTR_ENABLE_IPV6)
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s (oid 0x%" PRIx64 ") value %s",
                               attr.id, profile.profile_name.c_str(), profile.m_sai_ars_id, attr.value.booldata ? "true" : "false");
            }
            else
            {
                SWSS_LOG_WARN("Setting Attr %d is not supported. Failed to set ARS profile %s (oid 0x%" PRIx64 ") value %u",
                               attr.id, profile.profile_name.c_str(), profile.m_sai_ars_id, attr.value.u32);
            }
            continue;
        }

        status = sai_ars_profile_api->set_ars_profile_attribute(profile.m_sai_ars_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ars profile %s (oid 0x%" PRIx64 ") attr %d: %d",
                profile.profile_name.c_str(), profile.m_sai_ars_id, attr.id, status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    return true;
}

bool ArsOrch::deleteArsProfile(ArsProfileEntry &profile)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    if (profile.m_sai_ars_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("ARS profile %s has invalid SAI object ID; skipping delete",
                      profile.profile_name.c_str());
        return false;
    }

    sai_attribute_t switch_attr;
    switch_attr.id = SAI_SWITCH_ATTR_ARS_PROFILE;
    status = sai_switch_api->get_switch_attribute(gSwitchId , 1, &switch_attr);
    if (status == SAI_STATUS_SUCCESS && switch_attr.value.oid == profile.m_sai_ars_id)
    {
        switch_attr.value.oid = SAI_NULL_OBJECT_ID;
        status = sai_switch_api->set_switch_attribute(gSwitchId, &switch_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to unset switch ARS profile %s: %d",
                           profile.profile_name.c_str(), status);
        }
        else
        {
            SWSS_LOG_NOTICE("Unset switch ARS profile %s", profile.profile_name.c_str());
        }
    }
    SWSS_LOG_NOTICE("Deleting ARS profile %s (oid 0x%" PRIx64 ")",
                    profile.profile_name.c_str(), profile.m_sai_ars_id);

    status = sai_ars_profile_api->remove_ars_profile(profile.m_sai_ars_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete ARS profile %s (oid 0x%" PRIx64 "): status=%d",
                       profile.profile_name.c_str(), profile.m_sai_ars_id, status);

        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS_PROFILE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }

        return false;
    }

    profile.m_sai_ars_id = SAI_NULL_OBJECT_ID;
    profile.max_ecmp_groups = 0;
    profile.max_ecmp_members_per_group = 0;

    SWSS_LOG_NOTICE("Deleted ARS profile %s successfully", profile.profile_name.c_str());
    return true;
}


void ArsOrch::processArsInterfaces(bool enable, std::uint32_t scaling_factor)
{
    SWSS_LOG_ENTER();
    const auto &nh_member_list = gPortsOrch->getAllPorts();

    for (const auto &it : nh_member_list)
    {
        const auto &port = it.second;

        if (port.m_type != Port::PHY)
            continue;

        bool alreadyEnabled = (m_arsEnabledInterfaces.find(port.m_alias) != m_arsEnabledInterfaces.end());

        if (enable && !alreadyEnabled)
        {
            SWSS_LOG_INFO("Creating ARS interface for port: %s", port.m_alias.c_str());
            updateArsEnabledInterface(port, scaling_factor, true);
        }
        else if (!enable && alreadyEnabled)
        {
            SWSS_LOG_INFO("Removing ARS interface for port: %s", port.m_alias.c_str());
            updateArsEnabledInterface(port, scaling_factor, false);
        }
    }
}

void ArsOrch::createArsProfileSelectorMode(ArsProfileEntry &profile, ArsSelectorModeNhg new_nhg_mode, ArsSelectorModeNhg prev_nhg_mode)
{
    SWSS_LOG_ENTER();

    std::uint32_t scaling_factor = 0;

    if (new_nhg_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_GLOBAL)
    {
        SWSS_LOG_INFO("NHG ARS profile '%s' switched to GLOBAL mode. Creating ARS interfaces for all ports.",
                      profile.profile_name.c_str());
        processArsInterfaces(true, scaling_factor);
    }
    else if ((new_nhg_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE ||
              new_nhg_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_NEXTHOP) &&
             prev_nhg_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_GLOBAL)
    {
        SWSS_LOG_INFO("NHG ARS profile '%s' switched from GLOBAL to INTERFACE/NEXTHOP. Removing ARS interfaces from unused ports.",
                      profile.profile_name.c_str());
        processArsInterfaces(false, scaling_factor);
    }
}

std::vector<sai_attribute_t> ArsOrch::buildArsAttributesFromObject(ArsObjectEntry *object)
{
    SWSS_LOG_ENTER();

    std::vector<sai_attribute_t> ars_attrs;
    if (!object)
    {
        SWSS_LOG_WARN("ARS object is nullptr");
        return ars_attrs;
    }

    sai_attribute_t attr;

    // Check differences with prev applied values
    if (!m_arsObjects.empty())
    {
        auto it = m_arsObjects.find(object->ars_obj_name);
        if (it != m_arsObjects.end())
        {
            ArsObjectEntry &prev = it->second;

            if (object->assign_mode != prev.assign_mode)
            {
                attr.id = SAI_ARS_ATTR_MODE;
                attr.value.u32 = object->assign_mode;
                ars_attrs.push_back(attr);
            }

            if (object->flowlet_idle_time != prev.flowlet_idle_time)
            {
                attr.id = SAI_ARS_ATTR_IDLE_TIME;
                attr.value.u32 = object->flowlet_idle_time;
                ars_attrs.push_back(attr);
            }

            if (object->max_flows != prev.max_flows)
            {
                attr.id = SAI_ARS_ATTR_MAX_FLOWS;
                attr.value.u32 = object->max_flows;
                ars_attrs.push_back(attr);
            }

            if (object->primary_path_threshold != prev.primary_path_threshold)
            {
                attr.id = SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD;
                attr.value.u32 = object->primary_path_threshold;
                ars_attrs.push_back(attr);
            }

            if (object->alternative_path_cost != prev.alternative_path_cost)
            {
                attr.id = SAI_ARS_ATTR_ALTERNATE_PATH_COST;
                attr.value.u32 = object->alternative_path_cost;
                ars_attrs.push_back(attr);
            }

            return ars_attrs;
        }
    }

    /* first time create, fill everything */
    attr.id = SAI_ARS_ATTR_MODE;
    attr.value.u32 = object->assign_mode;
    ars_attrs.push_back(attr);

    attr.id = SAI_ARS_ATTR_IDLE_TIME;
    attr.value.u32 = object->flowlet_idle_time;
    ars_attrs.push_back(attr);

    attr.id = SAI_ARS_ATTR_MAX_FLOWS;
    attr.value.u32 = object->max_flows;
    ars_attrs.push_back(attr);

    attr.id = SAI_ARS_ATTR_PRIMARY_PATH_QUALITY_THRESHOLD;
    attr.value.u32 = object->primary_path_threshold;
    ars_attrs.push_back(attr);

    attr.id = SAI_ARS_ATTR_ALTERNATE_PATH_COST;
    attr.value.u32 = object->alternative_path_cost;
    ars_attrs.push_back(attr);

    return ars_attrs;
}



bool ArsOrch::createArsObject(ArsObjectEntry *object)
{
    SWSS_LOG_ENTER();

    if (!object)
    {
        SWSS_LOG_ERROR("createArsObject: null object pointer");
        return false;
    }

    auto ars_attrs = buildArsAttributesFromObject(object);
    std::vector<sai_attribute_t> supported_attrs;

    for (auto &attr : ars_attrs)
    {
        if (isCreateImplemented(SAI_OBJECT_TYPE_ARS, attr.id))
        {
            supported_attrs.push_back(attr);
            SWSS_LOG_NOTICE("ARS object: setting attr %d value %u", attr.id, attr.value.u32);
        }
        else
        {
            SWSS_LOG_WARN("Attr %d not supported for create ARS object", attr.id);
        }
    }

    if (supported_attrs.empty())
    {
        SWSS_LOG_WARN("No supported attrs for ARS object");
        return false;
    }

    sai_status_t status = sai_ars_api->create_ars(
        &object->ars_object_id,
        gSwitchId,
        (uint32_t)supported_attrs.size(),
        supported_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ARS object: %d", status);
        return parseHandleSaiStatusFailure(handleSaiSetStatus(SAI_API_ARS, status));
    }

    return true;
}

bool ArsOrch::setArsObject(ArsObjectEntry *object)
{
    SWSS_LOG_ENTER();

    if (!object)
    {
        SWSS_LOG_ERROR("setArsObject: null object pointer");
        return false;
    }

    auto ars_attrs = buildArsAttributesFromObject(object);
    sai_status_t status = SAI_STATUS_NOT_SUPPORTED;

    for (auto &a : ars_attrs)
    {
        if (!isSetImplemented(SAI_OBJECT_TYPE_ARS, a.id))
        {
            SWSS_LOG_WARN("Setting attr %d not supported. ARS oid 0x%" PRIx64 ")",
                          a.id, object->ars_object_id);
            continue;
        }

        status = sai_ars_api->set_ars_attribute(object->ars_object_id, &a);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set ARS attr %d: %d", a.id, status);

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

    const std::string &ars_obj_name = object->ars_obj_name;

    // Check 1: Is it a default ARS object for any profile
    for (const auto &profile_pair : m_arsProfiles)
    {
        const ArsProfileEntry &profile = profile_pair.second;

        if (profile.default_ars_object == ars_obj_name)
        {
            SWSS_LOG_WARN("ARS object %s is default ARS object for profile %s, cannot delete",
                      ars_obj_name.c_str(), profile.profile_name.c_str());
            return false;
        }

        if (profile.nhg_selector_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_GLOBAL)
        {
            SWSS_LOG_WARN("ARS object %s is GLOBAL in profile %s, cannot delete",
                          ars_obj_name.c_str(), profile.profile_name.c_str());
            return false;
        }
    }

    // Check 2: Is it used by any enabled interface
    for (const auto &iface : m_arsEnabledInterfaces)
    {
        const std::string &iface_ars = iface.second.second;
        if (iface_ars == ars_obj_name)
        {
            SWSS_LOG_WARN("ARS object %s is used by interface %s, cannot delete",
                          ars_obj_name.c_str(), iface.first.c_str());
            return false;
        }
    }

    // Check 3: Is it used by any nexthop
    for (const auto &nh : m_arsNexthops)
    {
        if (nh.second.ars_obj_name == ars_obj_name)
        {
            SWSS_LOG_WARN("ARS object %s is used by nexthop %s, cannot delete",
                          ars_obj_name.c_str(), nh.first.first.c_str());
            return false;
        }
    }

    status = sai_ars_api->remove_ars(object->ars_object_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ars object%s (oid 0x%" PRIx64 ": %d)",
                        object->ars_obj_name.c_str(), object->ars_object_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ARS, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool ArsOrch::updateArsEnabledInterface(const Port &port, const uint32_t scaling_factor, const bool is_enable)
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
        if (is_enable && !gPortsOrch->setPortArsLoadScaling(port, scaling_factor))
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

bool ArsOrch::findDefaultArsObject(const ArsSelectorModeNhg& selector_mode, std::string& ars_obj_name)
{
    SWSS_LOG_ENTER();
    for (const auto& profile_pair : m_arsProfiles)
    {
        const ArsProfileEntry& profile = profile_pair.second;

        if (profile.nhg_selector_mode == selector_mode &&
            !profile.default_ars_object.empty())
        {

            ars_obj_name = profile.default_ars_object;
            SWSS_LOG_INFO("Using default ars_obj_name '%s' from profile '%s'",
                         ars_obj_name.c_str(), profile.profile_name.c_str());
            return true;
        }
    }
    return false;
}

ArsSelectorModeNhg ArsOrch::parseNhgSelectorMode(const std::string &nhg_mode) const
{
    SWSS_LOG_ENTER();
    if (nhg_mode == "global")
    {
        return ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_GLOBAL;
    }
    else if (nhg_mode == "interface")
    {
        return ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE;
    }
    else if (nhg_mode == "nexthop")
    {
        return ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_NEXTHOP;
    }
    else
    {
        // Invalid mode
        return ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INVALID;
    }
}

ArsSelectorModeLag ArsOrch::parseLagSelectorMode(const std::string &modeStr) const
{
    SWSS_LOG_ENTER();
    if (modeStr == "global")
    {
        return ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_GLOBAL;
    }
    else if (modeStr == "interface")
    {
        return ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_INTERFACE;
    }
    else
    {
        // Invalid mode
        return ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_INVALID;
    }
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
        std::string             default_ars_object;
        ArsSelectorModeNhg nhg_mode = ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE;
        ArsSelectorModeNhg new_nhg_mode = ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE;
        ArsSelectorModeLag lag_mode = ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_INTERFACE;
        ArsSelectorModeLag new_lag_mode = ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_INTERFACE;

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
                nhg_mode = arsProfile_entry->second.nhg_selector_mode;
                lag_mode = arsProfile_entry->second.lag_selector_mode;
                default_ars_object = arsProfile_entry->second.default_ars_object;
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
                    if (fvValue(i) == "ewma")
                    {
                        algo = ARS_ALGORITHM_EWMA;
                    }
                    else
                    {
                        SWSS_LOG_WARN("Received unsupported algorithm %s", fvValue(i).c_str());
                        continue;
                    }
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
                else if (fvField(i) == ARS_FIELD_NAME_NHG_PATH_SELECTOR_MODE)
                {
                    new_nhg_mode = parseNhgSelectorMode(fvValue(i));
                    if (new_nhg_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INVALID)
                    {
                        SWSS_LOG_ERROR("Invalid NHG selector mode: %s", fvValue(i).c_str());
                        continue;
                    }
                    if(new_nhg_mode != nhg_mode)
                    {
                       createArsProfileSelectorMode(arsProfile_entry->second, new_nhg_mode, nhg_mode);
                       nhg_mode = new_nhg_mode;
                    }
                }
                else if (fvField(i) == ARS_FIELD_NAME_LAG_PATH_SELECTOR_MODE)
                {
                    new_lag_mode = parseLagSelectorMode(fvValue(i));
                    if (new_lag_mode == ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_INVALID)
                    {
                        SWSS_LOG_ERROR("Invalid LAG selector mode: %s", fvValue(i).c_str());
                        continue;
                    }
                }
                else if (fvField(i) == ARS_FIELD_NAME_DEFAULT_OBJECT)
                {
                    default_ars_object = fvValue(i);
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
            arsProfile_entry->second.nhg_selector_mode = nhg_mode;
            arsProfile_entry->second.lag_selector_mode = lag_mode;
            arsProfile_entry->second.default_ars_object = default_ars_object;

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
            if (new_nhg_mode == ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE)
            {
                if (!default_ars_object.empty())
                {
                    // Scan interface_table and update ars_object_name
                    for (auto &entry : m_arsEnabledInterfaces)
                    {
                        auto &ars_pair = entry.second;

                        // Update ars_object_name if not already set
                        if (ars_pair.second.empty())
                        {
                            ars_pair.second = default_ars_object;
                            SWSS_LOG_INFO("Updated ARS object for interface %s to default %s",
                                           entry.first.c_str(), default_ars_object.c_str());
                        }
                    }
                }
            }
            FieldValueTuple groups("max_ecmp_groups", std::to_string(arsProfile_entry->second.max_ecmp_groups));
            fvVector.push_back(groups);
            FieldValueTuple members("max_ecmp_members_per_group", std::to_string(arsProfile_entry->second.max_ecmp_members_per_group));
            fvVector.push_back(members);
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
                if (m_arsEnabledInterfaces.empty())
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
                deleteArsProfile(arsProfile_entry->second);
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
        Port p;
        std::uint32_t scaling_factor = 0;
        std::string ars_obj_name;

        SWSS_LOG_NOTICE("ARS Path Op %s Interface %s", op.c_str(), if_name.c_str());

        if (op == SET_COMMAND)
        {
            auto ars_if = m_arsEnabledInterfaces.find(if_name);

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == ARS_FIELD_NAME_SCALING_FACTOR)
                {
                    scaling_factor = static_cast<uint32_t>(stoi(fvValue(i)));

                    if (ars_if != m_arsEnabledInterfaces.end() && ars_if->second.first == scaling_factor)
                    {
                        SWSS_LOG_WARN("Scaling factor %s for interface %s unchanged - skipped",
                                      fvValue(i).c_str(), if_name.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }
                else if (fvField(i) == ARS_FIELD_NAME_OBJECT_NAME)
                {
                    ars_obj_name = fvValue(i);
                    if (!ars_obj_name.empty())
                    {
                        SWSS_LOG_INFO("Received ars_obj_name '%s' for interface %s",
                                     ars_obj_name.c_str(), if_name.c_str());
                    }
                    else
                    {
                        SWSS_LOG_INFO("Received empty ars_obj_name for interface %s", if_name.c_str());
                    }

                }
                else
                {
                    SWSS_LOG_WARN("Received unsupported field %s", fvField(i).c_str());
                    continue;
                }

            }

            /* Apply default ARS object if not provided */
            if (ars_obj_name.empty())
            {
                bool result = findDefaultArsObject(ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE, 
                                  ars_obj_name);
                if (!result)
                {
                   /* No suitable ars object found */
                    SWSS_LOG_WARN(
                    "NHG path selector is in INTERFACE mode, but no ARS object is configured "
                    "either in the ARS profile 'default_ars_object' or in the ARS interface 'ars_obj_name'");
                }
            }

            m_arsEnabledInterfaces[if_name] = std::make_pair(scaling_factor, ars_obj_name);
            SWSS_LOG_NOTICE("Added/Updated interface %s scaling_factor %d obj_name '%s'",
                            if_name.c_str(), scaling_factor, ars_obj_name.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            auto ars_if = m_arsEnabledInterfaces.find(if_name);
            if (ars_if == m_arsEnabledInterfaces.end())
            {
                SWSS_LOG_INFO("Delete called for non-existent interface %s", if_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            SWSS_LOG_INFO("Removing interface %s with obj_name '%s'",
                          if_name.c_str(), ars_if->second.second.c_str());
            m_arsEnabledInterfaces.erase(ars_if);
        }

        if (!gPortsOrch->getPort(if_name, p) || p.m_port_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Tried to %s non-existent/down interface %s - skipped", op.c_str(), if_name.c_str());
        }
        else
        {
            updateArsEnabledInterface(p, scaling_factor, (op == SET_COMMAND));
        }

        it = consumer.m_toSync.erase(it);
    }

  // Update ARS state table with scaling factor and ars_obj_name
    string ifnames = "";
    for (const auto &entry : m_arsEnabledInterfaces)
    {
        ifnames += entry.first + ":" + std::to_string(entry.second.first) +
                   ":" + entry.second.second + " "; // format: if_name:scaling:obj_name
    }
    return true;
}
bool ArsOrch::doTaskArsObject(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    bool status = false;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string ars_obj_name = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_NOTICE("ARS Object Op %s, Object Name %s", op.c_str(), ars_obj_name.c_str());

        ArsObjectEntry ars_object;

        if (op == SET_COMMAND)
        {
            // Set defaults
            ars_object.assign_mode = PER_FLOWLET_QUALITY;
            ars_object.flowlet_idle_time = ARS_OBJECT_DEFAULT_FLOWLET_IDLE_TIME_US;
            ars_object.max_flows = ARS_OBJECT_DEFAULT_MAX_FLOWS;
            ars_object.primary_path_threshold = ARS_OBJECT_DEFAULT_PRIMARY_PATH_THRESHOLD;
            ars_object.alternative_path_cost = ARS_OBJECT_DEFAULT_ALTERNATIVE_PATH_COST;

            // Override with incoming fields
            for (auto fv : kfvFieldsValues(t))
            {
                if (fvField(fv) == ARS_FIELD_NAME_ASSIGN_MODE)
                {
                    if (fvValue(fv) == "per_flowlet_quality")
                        ars_object.assign_mode = PER_FLOWLET_QUALITY;
                    else if (fvValue(fv) == "per_packet_quality")
                        ars_object.assign_mode = PER_PACKET_QUALITY;
                    else
                        SWSS_LOG_WARN("Unsupported assign_mode %s, using default", fvValue(fv).c_str());
                }
                else if (fvField(fv) == ARS_FIELD_NAME_IDLE_TIME)
                    ars_object.flowlet_idle_time = static_cast<uint16_t>(stoi(fvValue(fv)));
                else if (fvField(fv) == ARS_FIELD_NAME_OBJ_MAX_FLOWS)
                    ars_object.max_flows = static_cast<uint32_t>(stoi(fvValue(fv)));
                else if (fvField(fv) == ARS_FIELD_NAME_PRIMARY_PATH_THRESHOLD)
                    ars_object.primary_path_threshold = static_cast<uint32_t>(stoi(fvValue(fv)));
                else if (fvField(fv) == ARS_FIELD_NAME_ALTERNATIVE_PATH_COST)
                    ars_object.alternative_path_cost = static_cast<uint32_t>(stoi(fvValue(fv)));
            }

            // Create or update ARS object in SAI
            auto table_it = m_arsObjects.find(ars_obj_name);
            if (table_it == m_arsObjects.end())
            {
                /* Create new entry*/
                ars_object.ars_obj_name = ars_obj_name;
                status = createArsObject(&ars_object);
                if (status)
                {
                    SWSS_LOG_NOTICE("ARS object created: %s", ars_obj_name.c_str());
                }
            }
            else
            {
                /*Exisitng object so set the particular field*/
                ars_object.ars_object_id = table_it->second.ars_object_id;  // preserve oid
                status = setArsObject(&ars_object);
                if (status) 
                {
                    SWSS_LOG_NOTICE("ARS object updated: %s", ars_obj_name.c_str());
                }
            }
            if (status)
            {
                m_arsObjects[ars_obj_name] = ars_object;
            }
            else
            {
                SWSS_LOG_ERROR("Failed to create/update ARS object %s, skipping map update", ars_obj_name.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            auto table_it = m_arsObjects.find(ars_obj_name);
            if (table_it != m_arsObjects.end() && table_it->second.ars_object_id)
            {
                delArsObject(&table_it->second);
            }
            m_arsObjects.erase(ars_obj_name);
            SWSS_LOG_NOTICE("ARS object deleted: %s", ars_obj_name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }

    return true;
}

bool ArsOrch::doTaskArsNexthop(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        std::string key = kfvKey(t);

        /* Parse VRF and nexthop_ip from key*/
        size_t found = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        std::string vrf_name = key.substr(0, found);
        IpAddress nexthop_ip(key.substr(found + 1));
        std::string op = kfvOp(t);

        SWSS_LOG_NOTICE("ARS NHG Op %s, VRF %s, Nexthop %s",
                        op.c_str(), vrf_name.c_str(), nexthop_ip.to_string().c_str());

        auto nexthop_key = std::make_pair(vrf_name, nexthop_ip);

        if (op == SET_COMMAND)
        {

            // Find or create entry
            auto itr = m_arsNexthops.find(nexthop_key);
            if (itr == m_arsNexthops.end())
            {
                m_arsNexthops[nexthop_key] = ArsNexthopEntry();
                itr = m_arsNexthops.find(nexthop_key);
            }

            ArsNexthopEntry &ars_nexthop = itr->second;

            // Read fields
            std::string ars_obj_name = "";
            std::string role = "primary_path"; // default

            for (auto fv : kfvFieldsValues(t))
            {
                if (fvField(fv) == ARS_FIELD_NAME_ARS_OBJ_NAME)
                {
                    ars_obj_name = fvValue(fv);
                }
                else if (fvField(fv) == ARS_FIELD_NAME_ROLE)
                {
                    role = fvValue(fv);
                }
                else
                {
                    SWSS_LOG_WARN("Unsupported field %s in ARS_NEXTHOP_LIST",
                                  fvField(fv).c_str());
                }
            }

            /* Apply default ARS object if not provided */
            if (ars_obj_name.empty())
            {
                bool result = findDefaultArsObject(ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_NEXTHOP, 
                                  ars_obj_name);
                if (!result)
                {
                   /* No suitable profile found */
                    SWSS_LOG_WARN("NHG path selector is in nexthop mode, but no default ARS object is configured");
                }
            }

            /* Validate ARS object */
            auto ars_obj_it = m_arsObjects.find(ars_obj_name);
            if (ars_obj_it == m_arsObjects.end())
            {
                SWSS_LOG_ERROR("ARS object %s does not exist, cannot attach NHG",
                               ars_obj_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            ars_nexthop.ars_obj_name = ars_obj_name;
            ars_nexthop.role = role;

            SWSS_LOG_NOTICE("ARS NHG added: VRF %s, Nexthop %s, ARS Obj %s, Role %s",
                            vrf_name.c_str(), nexthop_ip.to_string().c_str(),
                            ars_obj_name.c_str(), role.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            auto itr = m_arsNexthops.find(nexthop_key);

            if (itr == m_arsNexthops.end())
            {
                SWSS_LOG_NOTICE("ARS NHG entry does not exist for VRF %s, Nexthop %s, ignoring delete",
                                vrf_name.c_str(), nexthop_ip.to_string().c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            ArsNexthopEntry &ars_nexthop = itr->second;

            // Remove from internal map
            m_arsNexthops.erase(itr);

            SWSS_LOG_NOTICE("ARS NHG removed: VRF %s, Nexthop %s, ARS Obj %s, Role %s",
                            vrf_name.c_str(), nexthop_ip.to_string().c_str(),
                            ars_nexthop.ars_obj_name.c_str(), ars_nexthop.role.c_str());
        }

        // Move iterator after processing
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

	if (table_name == CFG_ARS_PROFILE_TABLE_NAME)
    {
        doTaskArsProfile(consumer);
    }
    else if (table_name == CFG_ARS_INTERFACE_TABLE_NAME)
    {
        doTaskArsInterfaces(consumer);
    }
    else if (table_name == CFG_ARS_OBJECT_TABLE_NAME)
    {
        doTaskArsObject(consumer);
    }
    else if (table_name == CFG_ARS_NEXTHOP_TABLE_NAME)
    {
        doTaskArsNexthop(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table : %s", table_name.c_str());
    }
}
