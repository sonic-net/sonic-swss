#ifndef SWSS_UTIL_DROP_COUNTER_H_
#define SWSS_UTIL_DROP_COUNTER_H_

#include <string>
#include <unordered_set>
#include "debug_counter.h"
#include "drop_reasons.h"

extern "C" {
#include "sai.h"
}

using std::string;
using std::unordered_set;

// DropCounter represents a SAI debug counter object that track packet drops.
// It maintains a list of drop reasons and provides methods to add and remove
// drop reasons from the counter.
class DropCounter : public DebugCounter
{
    public:
        DropCounter(const string &counter_name, 
                    const string &counter_type,
                    const unordered_set<string> &drop_reasons);
        DropCounter(const DropCounter&) = delete;
        DropCounter& operator=(const DropCounter&) = delete;
        virtual ~DropCounter();

        const unordered_set<string> &getDropReasons() const { return drop_reasons; }

        virtual string getDebugCounterSAIStat() const;

        void addDropReason(const string &drop_reason);
        void removeDropReason(const string &drop_reason);

    private:
        void initializeDropCounterInSAI();
        void serializeDropReasons(uint32_t drop_reason_count, int32_t *drop_reason_list, sai_attribute_t *drop_reason_attribute);
        void updateDropReasonsInSAI();

        unordered_set<string> drop_reasons;
};

vector<string> getSupportedDropReasons(sai_debug_counter_attr_t drop_reason_type);
string serializeSupportedDropReasons(vector<string> drop_reasons);
uint64_t getSupportedDebugCounterAmounts(sai_debug_counter_type_t counter_type);

#endif // SWSS_UTIL_DROP_COUNTER_H_
