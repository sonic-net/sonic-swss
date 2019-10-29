#ifndef SWSS_UTIL_DROP_COUNTER_H_
#define SWSS_UTIL_DROP_COUNTER_H_

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "debug_counter.h"
#include "drop_reasons.h"

extern "C" {
#include "sai.h"
}

// TODO: Finish adding all the supported SAI drop reasons.
static std::unordered_map<std::string, sai_in_drop_reason_t> ingress_drop_reason_lookup = {
    { SMAC_EQUALS_DMAC,    SAI_IN_DROP_REASON_SMAC_EQUALS_DMAC },
    { SIP_LINK_LOCAL,      SAI_IN_DROP_REASON_SIP_LINK_LOCAL },
    { DIP_LINK_LOCAL,      SAI_IN_DROP_REASON_DIP_LINK_LOCAL },
    { L3_EGRESS_LINK_DOWN, SAI_IN_DROP_REASON_L3_EGRESS_LINK_DOWN },
    { ACL_ANY,             SAI_IN_DROP_REASON_ACL_ANY},
};

static std::unordered_map<std::string, sai_out_drop_reason_t> egress_drop_reason_lookup = {
    { L2_ANY,              SAI_OUT_DROP_REASON_L2_ANY },
    { EGRESS_VLAN_FILTER,  SAI_OUT_DROP_REASON_EGRESS_VLAN_FILTER },
    { L3_ANY,              SAI_OUT_DROP_REASON_L3_ANY },
    { L3_EGRESS_LINK_DOWN, SAI_OUT_DROP_REASON_L3_EGRESS_LINK_DOWN },
};

// DropCounter represents a SAI debug counter object that track packet drops.
class DropCounter : public DebugCounter
{
    public:
        DropCounter(const std::string &counter_name,
                    const std::string &counter_type,
                    const std::unordered_set<std::string> &drop_reasons);
        DropCounter(const DropCounter&) = delete;
        DropCounter& operator=(const DropCounter&) = delete;
        virtual ~DropCounter();

        const std::unordered_set<std::string> &getDropReasons() const { return drop_reasons; }

        virtual std::string getDebugCounterSAIStat() const;

        void addDropReason(const std::string &drop_reason);
        void removeDropReason(const std::string &drop_reason);

    private:
        void initializeDropCounterInSAI();
        void serializeDropReasons(uint32_t drop_reason_count, int32_t *drop_reason_list, sai_attribute_t *drop_reason_attribute);
        void updateDropReasonsInSAI();

        std::unordered_set<std::string> drop_reasons;
};

std::vector<std::string> getSupportedDropReasons(sai_debug_counter_attr_t drop_reason_type);
std::string serializeSupportedDropReasons(std::vector<std::string> drop_reasons);
uint64_t getSupportedDebugCounterAmounts(sai_debug_counter_type_t counter_type);

#endif // SWSS_UTIL_DROP_COUNTER_H_
