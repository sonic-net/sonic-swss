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

const std::unordered_map<std::string, sai_in_drop_reason_t> ingress_drop_reason_lookup = {
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

const std::unordered_map<std::string, sai_out_drop_reason_t> egress_drop_reason_lookup = {
    { L2_ANY,              SAI_OUT_DROP_REASON_L2_ANY },
    { EGRESS_VLAN_FILTER,  SAI_OUT_DROP_REASON_EGRESS_VLAN_FILTER },
    { L3_ANY,              SAI_OUT_DROP_REASON_L3_ANY },
    { L3_EGRESS_LINK_DOWN, SAI_OUT_DROP_REASON_L3_EGRESS_LINK_DOWN },
};

// DropCounter represents a SAI debug counter object that track packet drops.
class DropCounter : public DebugCounter
{
    public:
        DropCounter(const std::string& counter_name,
                    const std::string& counter_type,
                    const std::unordered_set<std::string>& drop_reasons);
        DropCounter(const DropCounter&) = delete;
        DropCounter& operator=(const DropCounter&) = delete;
        virtual ~DropCounter();

        const std::unordered_set<std::string>& getDropReasons() const { return drop_reasons; }

        virtual std::string getDebugCounterSAIStat() const;

        void addDropReason(const std::string& drop_reason);
        void removeDropReason(const std::string& drop_reason);

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
