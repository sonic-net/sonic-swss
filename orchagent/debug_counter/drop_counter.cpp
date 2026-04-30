#include "orchagent/debug_counter/drop_counter.h"

#include "logger.h"
#include "sai_serialize.h"

#include <vector>

using std::runtime_error;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

extern sai_object_id_t gSwitchId;
extern sai_debug_counter_api_t *sai_debug_counter_api;

#define INGRESS_DROP_REASON_PREFIX_LENGTH 19 // "SAI_IN_DROP_REASON_"
#define EGRESS_DROP_REASON_PREFIX_LENGTH  20 // "SAI_OUT_DROP_REASON_"

const unordered_map<string, sai_in_drop_reason_t> DropCounter::ingress_drop_reason_lookup =
{
    { L2_ANY,               SAI_IN_DROP_REASON_L2_ANY },
    { SMAC_MULTICAST,       SAI_IN_DROP_REASON_SMAC_MULTICAST },
    { SMAC_EQUALS_DMAC,     SAI_IN_DROP_REASON_SMAC_EQUALS_DMAC },
    { DMAC_RESERVED,        SAI_IN_DROP_REASON_DMAC_RESERVED },
    { VLAN_TAG_NOT_ALLOWED, SAI_IN_DROP_REASON_VLAN_TAG_NOT_ALLOWED },
    { INGRESS_VLAN_FILTER,  SAI_IN_DROP_REASON_INGRESS_VLAN_FILTER },
    { INGRESS_STP_FILTER,   SAI_IN_DROP_REASON_INGRESS_STP_FILTER },
    { FDB_UC_DISCARD,       SAI_IN_DROP_REASON_FDB_UC_DISCARD },
    { FDB_MC_DISCARD,       SAI_IN_DROP_REASON_FDB_MC_DISCARD },
    { L2_LOOPBACK_FILTER,   SAI_IN_DROP_REASON_L2_LOOPBACK_FILTER },
    { EXCEEDS_L2_MTU,       SAI_IN_DROP_REASON_EXCEEDS_L2_MTU },
    { L3_ANY,               SAI_IN_DROP_REASON_L3_ANY },
    { EXCEEDS_L3_MTU,       SAI_IN_DROP_REASON_EXCEEDS_L3_MTU },
    { TTL,                  SAI_IN_DROP_REASON_TTL },
    { L3_LOOPBACK_FILTER,   SAI_IN_DROP_REASON_L3_LOOPBACK_FILTER },
    { NON_ROUTABLE,         SAI_IN_DROP_REASON_NON_ROUTABLE },
    { NO_L3_HEADER,         SAI_IN_DROP_REASON_NO_L3_HEADER },
    { IP_HEADER_ERROR,      SAI_IN_DROP_REASON_IP_HEADER_ERROR },
    { UC_DIP_MC_DMAC,       SAI_IN_DROP_REASON_UC_DIP_MC_DMAC },
    { DIP_LOOPBACK,         SAI_IN_DROP_REASON_DIP_LOOPBACK },
    { SIP_LOOPBACK,         SAI_IN_DROP_REASON_SIP_LOOPBACK },
    { SIP_MC,               SAI_IN_DROP_REASON_SIP_MC },
    { SIP_CLASS_E,          SAI_IN_DROP_REASON_SIP_CLASS_E },
    { SIP_UNSPECIFIED,      SAI_IN_DROP_REASON_SIP_UNSPECIFIED },
    { MC_DMAC_MISMATCH,     SAI_IN_DROP_REASON_MC_DMAC_MISMATCH },
    { SIP_EQUALS_DIP,       SAI_IN_DROP_REASON_SIP_EQUALS_DIP },
    { SIP_BC,               SAI_IN_DROP_REASON_SIP_BC },
    { DIP_LOCAL,            SAI_IN_DROP_REASON_DIP_LOCAL },
    { DIP_LINK_LOCAL,       SAI_IN_DROP_REASON_DIP_LINK_LOCAL },
    { SIP_LINK_LOCAL,       SAI_IN_DROP_REASON_SIP_LINK_LOCAL },
    { IPV6_MC_SCOPE0,       SAI_IN_DROP_REASON_IPV6_MC_SCOPE0 },
    { IPV6_MC_SCOPE1,       SAI_IN_DROP_REASON_IPV6_MC_SCOPE1 },
    { IRIF_DISABLED,        SAI_IN_DROP_REASON_IRIF_DISABLED },
    { ERIF_DISABLED,        SAI_IN_DROP_REASON_ERIF_DISABLED },
    { LPM4_MISS,            SAI_IN_DROP_REASON_LPM4_MISS },
    { LPM6_MISS,            SAI_IN_DROP_REASON_LPM6_MISS },
    { BLACKHOLE_ROUTE,      SAI_IN_DROP_REASON_BLACKHOLE_ROUTE },
    { BLACKHOLE_ARP,        SAI_IN_DROP_REASON_BLACKHOLE_ARP },
    { UNRESOLVED_NEXT_HOP,  SAI_IN_DROP_REASON_UNRESOLVED_NEXT_HOP },
    { L3_EGRESS_LINK_DOWN,  SAI_IN_DROP_REASON_L3_EGRESS_LINK_DOWN },
    { DECAP_ERROR,          SAI_IN_DROP_REASON_DECAP_ERROR },
    { ACL_ANY,              SAI_IN_DROP_REASON_ACL_ANY},
    { ACL_INGRESS_PORT,     SAI_IN_DROP_REASON_ACL_INGRESS_PORT },
    { ACL_INGRESS_LAG,      SAI_IN_DROP_REASON_ACL_INGRESS_LAG },
    { ACL_INGRESS_VLAN,     SAI_IN_DROP_REASON_ACL_INGRESS_VLAN },
    { ACL_INGRESS_RIF,      SAI_IN_DROP_REASON_ACL_INGRESS_RIF },
    { ACL_INGRESS_SWITCH,   SAI_IN_DROP_REASON_ACL_INGRESS_SWITCH },
    { ACL_EGRESS_PORT,      SAI_IN_DROP_REASON_ACL_EGRESS_PORT },
    { ACL_EGRESS_LAG,       SAI_IN_DROP_REASON_ACL_EGRESS_LAG },
    { ACL_EGRESS_VLAN,      SAI_IN_DROP_REASON_ACL_EGRESS_VLAN },
    { ACL_EGRESS_RIF,       SAI_IN_DROP_REASON_ACL_EGRESS_RIF },
    { ACL_EGRESS_SWITCH,    SAI_IN_DROP_REASON_ACL_EGRESS_SWITCH }
};

const unordered_map<string, sai_out_drop_reason_t> DropCounter::egress_drop_reason_lookup =
{
    { L2_ANY,              SAI_OUT_DROP_REASON_L2_ANY },
    { EGRESS_VLAN_FILTER,  SAI_OUT_DROP_REASON_EGRESS_VLAN_FILTER },
    { L3_ANY,              SAI_OUT_DROP_REASON_L3_ANY },
    { L3_EGRESS_LINK_DOWN, SAI_OUT_DROP_REASON_L3_EGRESS_LINK_DOWN },
};

// Need to allocate enough space for the SAI to report the drop reasons, 100
// gives us plenty of space for both ingress and egress drop reasons.
const uint32_t maxDropReasons = 100;

// If initialization fails, this constructor will throw a runtime error.
DropCounter::DropCounter(const string& counter_name, const string& counter_type, const unordered_set<string>& drop_reasons)
        : DebugCounter(counter_name, counter_type), drop_reasons(drop_reasons)
{
    SWSS_LOG_ENTER();
    initializeDropCounterInSAI();
}

DropCounter::~DropCounter()
{
    SWSS_LOG_ENTER();
    try
    {
        DebugCounter::removeDebugCounterFromSAI();
    }
    catch (const std::runtime_error& e)
    {
        SWSS_LOG_ERROR("Failed to remove drop counter '%s' from SAI", name.c_str());
    }
}

// If we are unable to query the SAI or the type of counter is not supported
// then this method throws a runtime error.
std::string DropCounter::getDebugCounterSAIStat() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t index_attribute;
    index_attribute.id = SAI_DEBUG_COUNTER_ATTR_INDEX;
    if (sai_debug_counter_api->get_debug_counter_attribute(counter_id, 1, &index_attribute) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get stat for debug counter '%s'", name.c_str());
        throw runtime_error("Failed to get debug counter stat");
    }

    auto index = index_attribute.value.u32;
    if (type == PORT_INGRESS_DROPS)
    {
        return sai_serialize_port_stat(static_cast<sai_port_stat_t>(SAI_PORT_STAT_IN_DROP_REASON_RANGE_BASE + index));
    }
    else if (type == PORT_EGRESS_DROPS)
    {
        return sai_serialize_port_stat(static_cast<sai_port_stat_t>(SAI_PORT_STAT_OUT_DROP_REASON_RANGE_BASE + index));
    }
    else if (type == SWITCH_INGRESS_DROPS)
    {
        return sai_serialize_switch_stat(static_cast<sai_switch_stat_t>(SAI_SWITCH_STAT_IN_DROP_REASON_RANGE_BASE + index));
    }
    else if (type == SWITCH_EGRESS_DROPS)
    {
        return sai_serialize_switch_stat(static_cast<sai_switch_stat_t>(SAI_SWITCH_STAT_OUT_DROP_REASON_RANGE_BASE + index));
    }
    else
    {
        SWSS_LOG_ERROR("No stat found for debug counter '%s' of type '%s'", name.c_str(), type.c_str());
        throw runtime_error("No stat found for debug counter");
    }
}

// If the drop reason is already present on this counter, this method has no
// effect.
//
// If the update fails, this method throws a runtime error.
void DropCounter::addDropReason(const std::string& drop_reason)
{
    SWSS_LOG_ENTER();

    if (drop_reasons.find(drop_reason) != drop_reasons.end())
    {
        SWSS_LOG_DEBUG("Drop reason '%s' already present on '%s'", drop_reason.c_str(), name.c_str());
        return;
    }

    try
    {
        drop_reasons.emplace(drop_reason);
        updateDropReasonsInSAI();
    }
    catch (const std::runtime_error& e)
    {
        drop_reasons.erase(drop_reason);
        throw;
    }
}

// If the drop reason is not present on this counter, this method has no
// effect.
//
// If the update fails, this method throws a runtime error.
void DropCounter::removeDropReason(const std::string& drop_reason)
{
    SWSS_LOG_ENTER();

    auto drop_reason_it = drop_reasons.find(drop_reason);
    if (drop_reason_it == drop_reasons.end())
    {
        SWSS_LOG_DEBUG("Drop reason '%s' not present on '%s'", drop_reason.c_str(), name.c_str());
        return;
    }

    try
    {
        drop_reasons.erase(drop_reason_it);
        updateDropReasonsInSAI();
    }
    catch (const std::runtime_error& e)
    {
        drop_reasons.emplace(drop_reason);
        throw e;
    }
}

bool DropCounter::isIngressDropReasonValid(const std::string& drop_reason)
{
    return ingress_drop_reason_lookup.find(drop_reason) != ingress_drop_reason_lookup.end();
}

bool DropCounter::isEgressDropReasonValid(const std::string& drop_reason)
{
    return egress_drop_reason_lookup.find(drop_reason) != egress_drop_reason_lookup.end();
}

// If initialization fails for any reason, this method throws a runtime error.
void DropCounter::initializeDropCounterInSAI()
{
    sai_attribute_t debug_counter_attributes[2];
    vector<int32_t> drop_reason_list(drop_reasons.size());
    DebugCounter::serializeDebugCounterType(debug_counter_attributes[0]);
    DropCounter::serializeDropReasons(static_cast<uint32_t>(drop_reasons.size()), drop_reason_list.data(), debug_counter_attributes + 1);
    DebugCounter::addDebugCounterToSAI(2, debug_counter_attributes);
}

// serializeDropReasons takes the list of drop reasons associated with this
// counter and stores them in a SAI readable format in drop_reason_attribute.
//
// This method assumes that drop_reason_list points to a region in memory with
// enough space for drop_reason_count drop reasons to be stored.
//
// If any of the provided drop reasons (or their serialization) is undefined,
// then this method throws a runtime error.
void DropCounter::serializeDropReasons(uint32_t drop_reason_count, int32_t *drop_reason_list, sai_attribute_t *drop_reason_attribute)
{
    SWSS_LOG_ENTER();

    if (type == PORT_INGRESS_DROPS || type == SWITCH_INGRESS_DROPS)
    {
        drop_reason_attribute->id = SAI_DEBUG_COUNTER_ATTR_IN_DROP_REASON_LIST;
        drop_reason_attribute->value.s32list.count = drop_reason_count;
        drop_reason_attribute->value.s32list.list = drop_reason_list;

        int index = 0;
        for (auto drop_reason: drop_reasons)
        {
            auto reason_it = ingress_drop_reason_lookup.find(drop_reason);
            if (reason_it == ingress_drop_reason_lookup.end())
            {
                SWSS_LOG_ERROR("Ingress drop reason '%s' not found", drop_reason.c_str());
                throw runtime_error("Ingress drop reason not found");
            }

            drop_reason_list[index++] = static_cast<int32_t>(reason_it->second);
        }
    }
    else if (type == PORT_EGRESS_DROPS || type == SWITCH_EGRESS_DROPS)
    {
            drop_reason_attribute->id = SAI_DEBUG_COUNTER_ATTR_OUT_DROP_REASON_LIST;
            drop_reason_attribute->value.s32list.count = drop_reason_count;
            drop_reason_attribute->value.s32list.list = drop_reason_list;

            int index = 0;
            for (auto drop_reason: drop_reasons)
            {
                auto reason_it = egress_drop_reason_lookup.find(drop_reason);
                if (reason_it == egress_drop_reason_lookup.end())
                {
                    SWSS_LOG_ERROR("Egress drop reason '%s' not found", drop_reason.c_str());
                    throw runtime_error("Egress drop reason not found");
                }

                drop_reason_list[index++] = static_cast<int32_t>(reason_it->second);
            }
    }
    else
    {
        SWSS_LOG_ERROR("Serialization undefined for drop counter type '%s'", type.c_str());
        throw runtime_error("Failed to serialize drop counter attributes");
    }
}

// If the SAI update fails, this method throws a runtime error.
void DropCounter::updateDropReasonsInSAI()
{
    SWSS_LOG_ENTER();

    sai_attribute_t updated_drop_reasons;
    vector<int32_t> drop_reason_list(drop_reasons.size());
    serializeDropReasons(static_cast<uint32_t>(drop_reasons.size()), drop_reason_list.data(), &updated_drop_reasons);
    if (sai_debug_counter_api->set_debug_counter_attribute(counter_id, &updated_drop_reasons) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Could not update drop reasons for drop counter '%s'", name.c_str());
        throw runtime_error("Could not update drop reason list");
    }
}

// Multiple calls to this function are guaranteed to return the same reasons
// (assuming the device has not been rebooted between calls). The order of
// the list of reasons is not guaranteed to be the same between calls.
//
// If the device does not support querying drop reasons, this method will
// return an empty list.
unordered_set<string> DropCounter::getSupportedDropReasons(sai_debug_counter_attr_t drop_reason_type)
{
    sai_s32_list_t drop_reason_list;
    int32_t        supported_reasons[maxDropReasons];
    drop_reason_list.count = maxDropReasons;
    drop_reason_list.list = supported_reasons;

    if (sai_query_attribute_enum_values_capability(gSwitchId,
                                                   SAI_OBJECT_TYPE_DEBUG_COUNTER,
                                                   drop_reason_type,
                                                   &drop_reason_list) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("This device does not support querying drop reasons");
        return {};
    }

    unordered_set<string> supported_drop_reasons;
    for (uint32_t i = 0; i < drop_reason_list.count; i++)
    {
        string drop_reason;
        if (drop_reason_type == SAI_DEBUG_COUNTER_ATTR_IN_DROP_REASON_LIST)
        {
            drop_reason = sai_serialize_ingress_drop_reason(static_cast<sai_in_drop_reason_t>(drop_reason_list.list[i]));
            // in case of unsupported counter, enum value is returned as a str
            if (drop_reason.length() < INGRESS_DROP_REASON_PREFIX_LENGTH)
            {
                continue;
            }
            drop_reason = drop_reason.substr(INGRESS_DROP_REASON_PREFIX_LENGTH);
        }
        else
        {
            drop_reason = sai_serialize_egress_drop_reason(static_cast<sai_out_drop_reason_t>(drop_reason_list.list[i]));
            // in case of unsupported counter, enum value is returned as a str
            if (drop_reason.length() < EGRESS_DROP_REASON_PREFIX_LENGTH)
            {
                continue;
            }
            drop_reason = drop_reason.substr(EGRESS_DROP_REASON_PREFIX_LENGTH);
        }

        supported_drop_reasons.emplace(drop_reason);
    }

    return supported_drop_reasons;
}

// Returns a set of supported counter types.
unordered_set<string> DropCounter::getSupportedCounterTypes()
{
    sai_status_t status = SAI_STATUS_FAILURE;

    const auto& countersTypeLookup = getDebugCounterTypeLookup();
    unordered_set<string> supportedCounterTypes;

    sai_s32_list_t enumValuesCapabilities;
    vector<int32_t> saiCounterTypes;

    const auto* meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_DEBUG_COUNTER,
                                                      SAI_DEBUG_COUNTER_ATTR_TYPE);
    if (!meta)
    {
        SWSS_LOG_ERROR("SAI BUG: metadata null pointer returned by "
                       "sai_metadata_get_attr_metadata for SAI_DEBUG_COUNTER_ATTR_TYPE");
        return {};
    }

    if (!meta->isenum || !meta->enummetadata)
    {
        SWSS_LOG_ERROR("SAI BUG: SAI_DEBUG_COUNTER_ATTR_TYPE value type is not an enum");
        return {};
    }

    saiCounterTypes.assign(meta->enummetadata->valuescount, 0);

    enumValuesCapabilities.count = static_cast<uint32_t>(saiCounterTypes.size());
    enumValuesCapabilities.list = saiCounterTypes.data();

    status = sai_query_attribute_enum_values_capability(gSwitchId,
                                                        SAI_OBJECT_TYPE_DEBUG_COUNTER,
                                                        SAI_DEBUG_COUNTER_ATTR_TYPE,
                                                        &enumValuesCapabilities);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("This device does not support querying drop counters");
        return {};
    }

    for (uint32_t i = 0; i < enumValuesCapabilities.count; i++)
    {
        auto enumValue = static_cast<sai_debug_counter_type_t>(enumValuesCapabilities.list[i]);
        for (const auto& it: countersTypeLookup)
        {
            if (it.second == enumValue)
            {
                supportedCounterTypes.emplace(it.first);
                break;
            }
        }
    }

    return supportedCounterTypes;
}

// serializeSupportedDropReasons takes a list of drop reasons and returns that
// list as a string.
//
// e.g. { "SMAC_EQUALS_DMAC", "INGRESS_VLAN_FILTER" } -> "["SMAC_EQUALS_DMAC","INGRESS_VLAN_FILTER"]"
// e.g. { } -> "[]"
string DropCounter::serializeSupportedDropReasons(unordered_set<string> drop_reasons)
{
    if (drop_reasons.size() == 0)
    {
        return "[]";
    }

    string supported_drop_reasons;
    for (auto const& drop_reason : drop_reasons)
    {
        supported_drop_reasons += ',';
        supported_drop_reasons += drop_reason;
    }

    supported_drop_reasons[0] = '[';
    return supported_drop_reasons + ']';
}

// It is not guaranteed that the amount of available counters will change only
// if counters are added or removed. Depending on the platform, debug counters
// may share hardware resources with other ASIC objects in which case this
// amount may change due to resource allocation in other parts of the system.
//
// If the device does not support querying counter availability, this method
// will return 0.
uint64_t DropCounter::getSupportedDebugCounterAmounts(sai_debug_counter_type_t counter_type)
{
    sai_attribute_t attr;
    uint64_t count;

    attr.id = SAI_DEBUG_COUNTER_ATTR_TYPE;
    attr.value.s32 = counter_type;
    if (sai_object_type_get_availability(gSwitchId, SAI_OBJECT_TYPE_DEBUG_COUNTER, 1, &attr, &count) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("This device does not support querying the number of drop counters");
        return 0;
    }

    return count;
}
