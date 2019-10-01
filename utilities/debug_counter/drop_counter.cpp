#include "drop_counter.h"

#include <unordered_map>
#include "logger.h"
#include "sai_serialize.h"

using std::unordered_map;
using std::runtime_error;

extern sai_object_id_t gSwitchId;
extern sai_debug_counter_api_t *sai_debug_counter_api;

// TODO: Finish adding all the supported SAI drop reasons.
static unordered_map<string, sai_in_drop_reason_t> ingress_drop_reason_lookup = {
    { SMAC_EQUALS_DMAC,    SAI_IN_DROP_REASON_SMAC_EQUALS_DMAC },
    { SIP_LINK_LOCAL,      SAI_IN_DROP_REASON_SIP_LINK_LOCAL },
    { DIP_LINK_LOCAL,      SAI_IN_DROP_REASON_DIP_LINK_LOCAL },
    { L3_EGRESS_LINK_DOWN, SAI_IN_DROP_REASON_L3_EGRESS_LINK_DOWN },
    { ACL_ANY,             SAI_IN_DROP_REASON_ACL_ANY},
};

static unordered_map<string, sai_out_drop_reason_t> egress_drop_reason_lookup = {
    { L2_ANY,              SAI_OUT_DROP_REASON_L2_ANY },
    { EGRESS_VLAN_FILTER,  SAI_OUT_DROP_REASON_EGRESS_VLAN_FILTER },
    { L3_ANY,              SAI_OUT_DROP_REASON_L3_ANY },
    { L3_EGRESS_LINK_DOWN, SAI_OUT_DROP_REASON_L3_EGRESS_LINK_DOWN },
};

// DropCounter initializes a new debug counter for tracking packet drops.
//
// If initialization fails, this constructor will throw a runtime error.
DropCounter::DropCounter(const string &counter_name, const string &counter_type, const unordered_set<string> &drop_reasons) 
        : DebugCounter(counter_name, counter_type), drop_reasons(drop_reasons) 
{
    SWSS_LOG_ENTER();
    this->initializeDropCounterInSAI();
}

// ~DropCounter deletes this drop counter from the SAI.
DropCounter::~DropCounter() 
{
    SWSS_LOG_ENTER();
    try
    {
        DebugCounter::removeDebugCounterFromSAI();
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("Failed to remove drop counter '%s' from SAI", this->name.c_str());
    }
}

// getDebugCounterSAIStat returns the name of the stat in the SAI that contains 
// the data for this drop counter.
//
// If we are unable to query the SAI or the type of counter is not supported
// then this method throws a runtime error.
std::string DropCounter::getDebugCounterSAIStat() const 
{
    SWSS_LOG_ENTER();

    sai_attribute_t index_attribute;
    index_attribute.id = SAI_DEBUG_COUNTER_ATTR_INDEX;
    if (sai_debug_counter_api->get_debug_counter_attribute(this->counter_id, 1, &index_attribute) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get stat for debug counter '%s'", this->name.c_str());
        throw runtime_error("Failed to get debug counter stat");
    }

    auto index = index_attribute.value.u32;
    if (this->type == PORT_INGRESS_DROPS)
    {
        return sai_serialize_port_stat(static_cast<sai_port_stat_t>(SAI_PORT_STAT_IN_DROP_REASON_RANGE_BASE + index));
    }
    else if (this->type == PORT_EGRESS_DROPS)
    {
        return sai_serialize_port_stat(static_cast<sai_port_stat_t>(SAI_PORT_STAT_OUT_DROP_REASON_RANGE_BASE + index));
    }
    else if (this->type == SWITCH_INGRESS_DROPS)
    {
        return sai_serialize_switch_stat(static_cast<sai_switch_stat_t>(SAI_SWITCH_STAT_IN_DROP_REASON_RANGE_BASE + index));
    }
    else if (this->type == SWITCH_EGRESS_DROPS)
    {
        return sai_serialize_switch_stat(static_cast<sai_switch_stat_t>(SAI_SWITCH_STAT_OUT_DROP_REASON_RANGE_BASE + index));
    }
    else
    {
        SWSS_LOG_ERROR("No stat found for debug counter '%s' of type '%s'", this->name.c_str(), this->type.c_str());
        throw runtime_error("No stat found for debug counter");
    }
}

// addDropReason adds a drop reason to this drop counter.
// 
// If the drop reason is already present on this counter, this method has no
// effect.
//
// If the update fails, this method throws a runtime error.
void DropCounter::addDropReason(const std::string &drop_reason)
{
    SWSS_LOG_ENTER();

    if (this->drop_reasons.find(drop_reason) != this->drop_reasons.end())
    {
        SWSS_LOG_DEBUG("Drop reason '%s' already present on '%s'", drop_reason.c_str(), this->name.c_str());
        return;
    }
    
    this->drop_reasons.emplace(drop_reason);
    this->updateDropReasonsInSAI();
}

// removeDropReason removes a drop reason from this drop counter.
//
// If the drop reason is not present on this counter, this method has no
// effect.
//
// If the update fails, this method throws a runtime error.
void DropCounter::removeDropReason(const std::string &drop_reason)
{
    SWSS_LOG_ENTER();

    auto drop_reason_it = this->drop_reasons.find(drop_reason);
    if (drop_reason_it == this->drop_reasons.end())
    {
        SWSS_LOG_DEBUG("Drop reason '%s' not present on '%s'", drop_reason.c_str(), this->name.c_str());
        return;
    }
    
    this->updateDropReasonsInSAI();
    this->drop_reasons.erase(drop_reason_it);
}

// initializeDropCounterInSAI initializes this drop counter in the SAI.
//
// If initialization fails for any reason, this method throws a runtime error.
void DropCounter::initializeDropCounterInSAI() 
{
    sai_attribute_t debug_counter_attributes[2];
    vector<int32_t> drop_reason_list(this->drop_reasons.size());
    DebugCounter::serializeDebugCounterMetadata(debug_counter_attributes);
    DropCounter::serializeDropReasons(static_cast<uint32_t>(this->drop_reasons.size()), drop_reason_list.data(), debug_counter_attributes + 1);
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

    if (this->type == PORT_INGRESS_DROPS || this->type == SWITCH_INGRESS_DROPS) 
    {
        drop_reason_attribute->id = SAI_DEBUG_COUNTER_ATTR_IN_DROP_REASON_LIST;
        drop_reason_attribute->value.s32list.count = drop_reason_count;
        drop_reason_attribute->value.s32list.list = drop_reason_list;

        int index = 0;
        for (auto drop_reason: this->drop_reasons) 
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
    else if (this->type == PORT_EGRESS_DROPS || this->type == SWITCH_EGRESS_DROPS) 
    {
            drop_reason_attribute->id = SAI_DEBUG_COUNTER_ATTR_OUT_DROP_REASON_LIST;
            drop_reason_attribute->value.s32list.count = drop_reason_count;
            drop_reason_attribute->value.s32list.list = drop_reason_list;

            int index = 0;
            for (auto drop_reason: this->drop_reasons) 
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
        SWSS_LOG_ERROR("Serialization undefined for drop counter type '%s'", this->type.c_str());
        throw runtime_error("Failed to serialize drop counter attributes");
    }
}

// updateDropReasonsInSAI will push the set of drop reasons that are currently
// attached to this counter to the SAI.
// 
// If the SAI update fails, this method throws a runtime error.
void DropCounter::updateDropReasonsInSAI()
{
    SWSS_LOG_ENTER();

    sai_attribute_t updated_drop_reasons;
    vector<int32_t> drop_reason_list(this->drop_reasons.size());
    this->serializeDropReasons(static_cast<uint32_t>(this->drop_reasons.size()), drop_reason_list.data(), &updated_drop_reasons);
    if (sai_debug_counter_api->set_debug_counter_attribute(this->counter_id, &updated_drop_reasons) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Could not update drop reasons for drop counter '%s'", this->name.c_str());
        throw runtime_error("Could not update drop reason list");
    }
}

// FREE FUNCTIONS START HERE ----------------------------------------------------------------------

// getSupportedDropReasons queries the SAI and returns a list of the drop
// reasons that are supported by this device.
// 
// Multiple calls to this function are guaranteed to return the same reasons
// (assuming the device has not been rebooted between calls). The order of
// the list of reasons is not guaranteed to be the same between calls.
//
// If the device does not support querying drop reasons, this method will 
// return an empty list.
vector<string> getSupportedDropReasons(sai_debug_counter_attr_t drop_reason_type) 
{
    sai_s32_list_t drop_reason_list;
    int32_t        supported_reasons[100];
    drop_reason_list.count = 100;
    drop_reason_list.list = supported_reasons;

    if (sai_query_attribute_enum_values_capability(gSwitchId, 
                                                   SAI_OBJECT_TYPE_DEBUG_COUNTER, 
                                                   drop_reason_type,
                                                   &drop_reason_list) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("This device does not support querying drop reasons");
        return {};
    }

    vector<string> supported_drop_reasons;
    for (uint32_t i = 0; i < drop_reason_list.count; i++) 
    {
        string drop_reason;
        if (drop_reason_type == SAI_DEBUG_COUNTER_ATTR_IN_DROP_REASON_LIST)
        {
            drop_reason = sai_serialize_ingress_drop_reason(drop_reason_list.list[i]);
        }
        else
        {
            drop_reason = sai_serialize_egress_drop_reason(drop_reason_list.list[i]);
        }
        
        supported_drop_reasons.push_back(drop_reason);
    }

    return supported_drop_reasons;
}

// serializeSupportedDropReasons takes a list of drop reasons and returns that
// list as a string.
//
// e.g. { "SMAC_EQUALS_DMAC", "INGRESS_VLAN_FILTER" } -> "["SMAC_EQUALS_DMAC","INGRESS_VLAN_FILTER"]"
// e.g. { } -> "[]"
string serializeSupportedDropReasons(vector<string> drop_reasons)
{
    if (drop_reasons.size() == 0) 
    {
        return "[]";
    }
    
    string supported_drop_reasons;
    for (auto const &drop_reason : drop_reasons)
    {
        supported_drop_reasons += ',';
        supported_drop_reasons += drop_reason;
    }

    supported_drop_reasons[0] = '[';
    return supported_drop_reasons + ']';
}

// getSupportedDebugCounterAmounts returns the number of currently available
// debug counters of a given type.
//
// It is not guaranteed that the amount of available counters will change only
// if counters are added or removed. Depending on the platform, debug counters
// may share hardware resources with other ASIC objects in which case this
// amount may change due to resource allocation in other parts of the system.
//
// If the device does not support querying counter availability, this method
// will return 0.
uint64_t getSupportedDebugCounterAmounts(sai_debug_counter_type_t counter_type)
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
