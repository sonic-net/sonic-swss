#ifndef SWSS_UTIL_DROP_COUNTER_H_
#define SWSS_UTIL_DROP_COUNTER_H_

#include <string>
#include <unordered_set>
#include <unordered_map>
#include "debug_counter.h"
#include "drop_reasons.h"

extern "C" {
#include "sai.h"
}

// DropCounter represents a SAI debug counter object that track packet drops.
class DropCounter : public DebugCounter
{
    public:
        DropCounter(const std::string& counter_name,
                    const std::string& counter_type,
                    const std::unordered_set<std::string>& drop_reasons) noexcept(false);
        DropCounter(const DropCounter&) = delete;
        DropCounter& operator=(const DropCounter&) = delete;
        virtual ~DropCounter();

        const std::unordered_set<std::string>& getDropReasons() const { return drop_reasons; }

        virtual std::string getDebugCounterSAIStat() const noexcept(false);

        void addDropReason(const std::string& drop_reason) noexcept(false);
        void removeDropReason(const std::string& drop_reason) noexcept(false);

        static bool isIngressDropReasonValid(const std::string& drop_reason);
        static bool isEgressDropReasonValid(const std::string& drop_reason);

        static std::unordered_set<std::string> getSupportedDropReasons(sai_debug_counter_attr_t drop_reason_type);
        static std::string serializeSupportedDropReasons(std::unordered_set<std::string> drop_reasons);
        static uint64_t getSupportedDebugCounterAmounts(sai_debug_counter_type_t counter_type);

    private:
        void initializeDropCounterInSAI() noexcept(false);
        void serializeDropReasons(
                uint32_t drop_reason_count,
                int32_t *drop_reason_list,
                sai_attribute_t *drop_reason_attribute) noexcept(false);
        void updateDropReasonsInSAI() noexcept(false);

        std::unordered_set<std::string> drop_reasons;

        static const std::unordered_map<std::string, sai_in_drop_reason_t> ingress_drop_reason_lookup;
        static const std::unordered_map<std::string, sai_out_drop_reason_t> egress_drop_reason_lookup;
};

#endif // SWSS_UTIL_DROP_COUNTER_H_
