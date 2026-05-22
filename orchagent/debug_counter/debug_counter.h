#ifndef SWSS_UTIL_DEBUG_COUNTER_H_
#define SWSS_UTIL_DEBUG_COUNTER_H_

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <inttypes.h>


extern "C" {
#include "sai.h"
}

// Supported debug counter attributes.
#define COUNTER_ALIAS       "alias"
#define COUNTER_TYPE        "type"
#define COUNTER_DESCRIPTION "desc"
#define COUNTER_GROUP       "group"

// Drop counter specific attributes for drop monitor feature.
#define DROP_MONITOR_STATUS                   "drop_monitor_status"
#define DROP_MONITOR_DROP_COUNT_THRESHOLD     "drop_count_threshold"
#define DROP_MONITOR_INCIDENT_COUNT_THRESHOLD "incident_count_threshold"
#define DROP_MONITOR_WINDOW                   "window"

// Supported debug counter types.
#define PORT_INGRESS_DROPS   "PORT_INGRESS_DROPS"
#define PORT_EGRESS_DROPS    "PORT_EGRESS_DROPS"
#define SWITCH_INGRESS_DROPS "SWITCH_INGRESS_DROPS"
#define SWITCH_EGRESS_DROPS  "SWITCH_EGRESS_DROPS"

// DebugCounter represents a SAI debug counter object.
class DebugCounter
{
    public:
        DebugCounter(const std::string& counter_name, const std::string& counter_type) noexcept(false);
        DebugCounter(const DebugCounter&) = delete;
        DebugCounter& operator=(const DebugCounter&) = delete;
        virtual ~DebugCounter();

        std::string getCounterName() const { return name; }
        std::string getCounterType() const { return type; }

        virtual std::string getDebugCounterSAIStat() const noexcept(false) = 0;

        static const std::unordered_set<std::string>& getSupportedDebugCounterAttributes()
        {
            return supported_debug_counter_attributes;
        }

        // TODO: We should try to neatly abstract this like we've done for the isValid methods in DropCounter.
        static const std::unordered_map<std::string, sai_debug_counter_type_t>& getDebugCounterTypeLookup()
        {
            return debug_counter_type_lookup;
        }

    protected:
        // These methods are intended to help with initialization. Dervied types will most likely
        // need to define additional helper methods to serialize additional fields (see DropCounter for example).
        void serializeDebugCounterType(sai_attribute_t& type_attribute);
        void addDebugCounterToSAI(int num_attrs, const sai_attribute_t *counter_attrs) noexcept(false);
        void removeDebugCounterFromSAI() noexcept(false);

        std::string name;
        std::string type;
        sai_object_id_t counter_id = 0;

        static const std::unordered_set<std::string> supported_debug_counter_attributes;
        static const std::unordered_map<std::string, sai_debug_counter_type_t> debug_counter_type_lookup;
};

#endif // _SWSS_UTIL_DEBUG_COUNTER_H_
