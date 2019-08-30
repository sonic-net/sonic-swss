#include "debug_counter.h"
#include "drop_counter.h"

#include <exception>
#include <unordered_map>
#include "logger.h"

using std::runtime_error;
using std::unordered_map;

extern sai_object_id_t gSwitchId;
extern sai_debug_counter_api_t *sai_debug_counter_api;

// DebugCounter initializes the name and type of this counter.
// It is expected that derived types populate any relevant fields and initialize the counter in the SAI.
//
// counter_name: The user-assigned name for this counter.
// counter_type: The type of this counter. If this type is not a member of debug_counter_type_lookup
//               then this constructor will throw a runtime error.
DebugCounter::DebugCounter(const string &counter_name, const string &counter_type) : name(counter_name) {
    SWSS_LOG_ENTER();

    auto counter_type_it = debug_counter_type_lookup.find(counter_type);
    if (counter_type_it == debug_counter_type_lookup.end()) {
        SWSS_LOG_ERROR("Failed to initialize debug counter of type '%s'", counter_type.c_str());
        throw runtime_error("Failed to initialize debug counter");
    }
    this->type = counter_type_it->first;
}

// ~DebugCounter uninitializes this counter.
// It is expected that derived types delete this counter from the SAI.
DebugCounter::~DebugCounter() {
    SWSS_LOG_ENTER();
}

// serializeDebugCounterMetadata populates the type field in serialized_attributes.
//
// If this type does not have a mapping in debug_counter_type_lookup then it will throw
// a runtime error.
//
// serialized_attributes: A pointer to an array of SAI attributes for this debug counter.
//                        Behavior is undefined if the length of serialized_attributes is
//                        less than 1.
void DebugCounter::serializeDebugCounterMetadata(sai_attribute_t *serialized_attributes) {
    SWSS_LOG_ENTER();

    auto type_it = debug_counter_type_lookup.find(this->type);
    if (type_it == debug_counter_type_lookup.end()) {
        SWSS_LOG_ERROR("Debug counter type '%s' not found", this->type.c_str());
        throw std::runtime_error("Failed to serialize debug counter attributes");
    }
    SWSS_LOG_DEBUG("Serializing debug counter of type '%s'", this->type.c_str());

    sai_debug_counter_type_t sai_counter_type = type_it->second;
    serialized_attributes[0].id = SAI_DEBUG_COUNTER_ATTR_TYPE;
    serialized_attributes[0].value.s32 = sai_counter_type;
}

// addDebugCounterToSAI creates a new debug counter object in the SAI given a list of debug counter attributes.
//
// If the SAI returns an error then this method will throw a runtime error.
//
// num_attributes: The number of attributes in debug_counter_attributes. Behavior is undefined if num_attributes
//                 is not equal to the number of attributes in debug_counter_attributes.
// debug_counter_attributes: A list of attributes defining the new debug counter object.
void DebugCounter::addDebugCounterToSAI(const int num_attributes, const sai_attribute_t *debug_counter_attributes) {
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("Adding debug counter '%s' to SAI", this->name.c_str());
    sai_object_id_t debug_counter_id;
    if (sai_debug_counter_api->create_debug_counter(&debug_counter_id, 
                                                    gSwitchId, 
                                                    num_attributes, 
                                                    debug_counter_attributes) != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to create debug counter '%s'", this->name.c_str());
        throw std::runtime_error("Failed to create debug counter");
    }

    SWSS_LOG_DEBUG("Created debug counter '%s' with OID=%lu", this->name.c_str(), debug_counter_id);
    this->counter_id = debug_counter_id;
}

// removeDebugCounterFromSAI deletes a debug counter object from the SAI.
void DebugCounter::removeDebugCounterFromSAI() {
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("Removing debug counter '%s' from SAI", this->name.c_str());
    if (sai_debug_counter_api->remove_debug_counter(counter_id) != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to remove debug counter '%s'", this->name.c_str());
        throw std::runtime_error("Failed to remove debug counter");
    }
}
