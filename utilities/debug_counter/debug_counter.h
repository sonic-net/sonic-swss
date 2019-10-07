#ifndef SWSS_UTIL_DEBUG_COUNTER_H_
#define SWSS_UTIL_DEBUG_COUNTER_H_

#include <string>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "sai.h"
}

// Supported debug counter attributes.
#define COUNTER_ALIAS       "alias"
#define COUNTER_TYPE        "type"
#define COUNTER_DESCRIPTION "desc"
#define COUNTER_GROUP       "group"

// Set of supported attributes to support easy look-up.
static const std::unordered_set<std::string> supported_debug_counter_attributes =
{
    COUNTER_ALIAS,
    COUNTER_TYPE,
    COUNTER_DESCRIPTION,
    COUNTER_GROUP
};

// Supported debug counter types.
#define PORT_INGRESS_DROPS   "PORT_INGRESS_DROPS"
#define PORT_EGRESS_DROPS    "PORT_EGRESS_DROPS"
#define SWITCH_INGRESS_DROPS "SWITCH_INGRESS_DROPS"
#define SWITCH_EGRESS_DROPS  "SWITCH_EGRESS_DROPS"

static const std::unordered_map<std::string, sai_debug_counter_type_t> debug_counter_type_lookup = {
    { PORT_INGRESS_DROPS, SAI_DEBUG_COUNTER_TYPE_PORT_IN_DROP_REASONS },
    { PORT_EGRESS_DROPS,  SAI_DEBUG_COUNTER_TYPE_PORT_OUT_DROP_REASONS },
    { SWITCH_INGRESS_DROPS, SAI_DEBUG_COUNTER_TYPE_SWITCH_IN_DROP_REASONS },
    { SWITCH_EGRESS_DROPS, SAI_DEBUG_COUNTER_TYPE_SWITCH_OUT_DROP_REASONS }
};

// DebugCounter represents a SAI debug counter object.
class DebugCounter
{
    public:
        DebugCounter(const std::string &counter_name, const std::string &counter_type);
        DebugCounter(const DebugCounter&) = delete;
        DebugCounter& operator=(const DebugCounter&) = delete;
        virtual ~DebugCounter();

        std::string getCounterName() const { return this->name; }
        std::string getCounterType() const { return this->type; }

        virtual std::string getDebugCounterSAIStat() const = 0;

    protected:
        // These methods are intended to help with initialization. Dervied types will most likely
        // need to define additional helper methods to serialize additional fields (see DropCounter for example).
        void serializeDebugCounterMetadata(sai_attribute_t *serialized_attributes);
        void addDebugCounterToSAI(int num_attrs, const sai_attribute_t *counter_attrs);
        void removeDebugCounterFromSAI();

        std::string name;
        std::string type;
        sai_object_id_t counter_id = 0;
};

#endif // _SWSS_UTIL_DEBUG_COUNTER_H_
