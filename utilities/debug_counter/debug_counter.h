#ifndef SWSS_UTIL_DEBUG_COUNTER_H_
#define SWSS_UTIL_DEBUG_COUNTER_H_

#include <unordered_set>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include "rediscommand.h"

extern "C" {
#include "sai.h"
}

using std::unordered_set;
using std::unordered_map;
using std::string;
using std::unique_ptr;
using std::vector;
using swss::FieldValueTuple;

// Supported debug counter attributes.
#define COUNTER_ALIAS       "alias"
#define COUNTER_TYPE        "type"
#define COUNTER_DESCRIPTION "desc"
#define COUNTER_GROUP       "group"

// Set of supported attributes to support easy look-up.
static const unordered_set<string> supported_debug_counter_attributes =
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

// A mapping from SONiC debug counter names to SAI debug counter names.
static const unordered_map<string, sai_debug_counter_type_t> debug_counter_type_lookup = {
    { PORT_INGRESS_DROPS, SAI_DEBUG_COUNTER_TYPE_PORT_IN_DROP_REASONS },
    { PORT_EGRESS_DROPS,  SAI_DEBUG_COUNTER_TYPE_PORT_OUT_DROP_REASONS },
    { SWITCH_INGRESS_DROPS, SAI_DEBUG_COUNTER_TYPE_SWITCH_IN_DROP_REASONS },
    { SWITCH_EGRESS_DROPS, SAI_DEBUG_COUNTER_TYPE_SWITCH_OUT_DROP_REASONS }
};

// DebugCounter represents a SAI debug counter object. Every SAI debug counter
// has a name, a type, and a stat that you can query to check the counts. Derived
// types of DebugCounter (e.g. DropCounter) may provide additional fields and
// behavior on top of the standard DebugCounter interface.
//
// Each DebugCounter object maps to one, and only one, SAI debug counter. As
// such, DebugCounters cannot be copied, moved, or otherwise mutated once
// initialized. It is suggested to use smart pointers to manage them.
//
// Initializing a SAI debug counter is handled in the constructor of the derived type. 
// If this call fails, the constructor will throw an exception to indicate so. Deleting 
// the debug counter is handled in the destructor. It is assumed that this operation 
// cannot fail, and doing so would represent an irrecoverable error.
class DebugCounter
{
    public:
        DebugCounter(const string &counter_name, const string &counter_type);
        DebugCounter(const DebugCounter&) = delete;
        DebugCounter& operator=(const DebugCounter&) = delete;
        virtual ~DebugCounter();

        string getCounterName() const { return this->name; }
        string getCounterType() const { return this->type; }

        virtual string getDebugCounterSAIStat() const = 0;

    protected:
        // These methods are intended to help with initialization. Dervied types will most likely
        // need to define additional helper methods to serialize additional fields (see DropCounter for example).
        void serializeDebugCounterMetadata(sai_attribute_t *serialized_attributes);
        void addDebugCounterToSAI(int num_attrs, const sai_attribute_t *counter_attrs);
        void removeDebugCounterFromSAI();

        string name;
        string type;
        sai_object_id_t counter_id = 0;
};

#endif // _SWSS_UTIL_DEBUG_COUNTER_H_
