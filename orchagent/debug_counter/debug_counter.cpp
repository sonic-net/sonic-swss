#include "debug_counter.h"
#include "drop_counter.h"

#include <unordered_set>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include "rediscommand.h"
#include <exception>
#include "logger.h"

using std::runtime_error;
using std::string;
using std::unordered_map;
using std::unique_ptr;
using std::unordered_set;
using std::vector;
using swss::FieldValueTuple;

extern sai_object_id_t gSwitchId;
extern sai_debug_counter_api_t *sai_debug_counter_api;

// It is expected that derived types populate any relevant fields and
// initialize the counter in the SAI.
//
// If counter_type is not a member of debug_counter_type_lookup then this
// constructor will throw a runtime error.
DebugCounter::DebugCounter(
        const string& counter_name,
        const string& counter_type)
    : name(counter_name)
{
    SWSS_LOG_ENTER();

    auto counter_type_it = debug_counter_type_lookup.find(counter_type);
    if (counter_type_it == debug_counter_type_lookup.end()) {
        SWSS_LOG_ERROR("Failed to initialize debug counter of type '%s'",
                counter_type.c_str());
        throw runtime_error("Failed to initialize debug counter");
    }
    this->type = counter_type_it->first;
}

// It is expected that derived types delete the counter from the SAI.
DebugCounter::~DebugCounter()
{
    SWSS_LOG_ENTER();
}

// serializeDebugCounterMetadata populates ONLY the type field in
// serialized_attributes.
//
// Behavior is undefined if the length of serialized_attributes is less than 1.
void DebugCounter::serializeDebugCounterMetadata(sai_attribute_t *serialized_attributes)
{
    SWSS_LOG_ENTER();

    sai_debug_counter_type_t sai_counter_type = debug_counter_type_lookup.at(this->type);;
    serialized_attributes[0].id = SAI_DEBUG_COUNTER_ATTR_TYPE;
    serialized_attributes[0].value.s32 = sai_counter_type;

    SWSS_LOG_DEBUG("Serializing debug counter of type '%s'", this->type.c_str());
}

// addDebugCounterToSAI creates a new debug counter object in the SAI given a list of debug counter attributes.
//
// If the SAI returns an error then this method will throw a runtime error.
//
// Behavior is undefined if num_attributes is not equal to the number of
// attributes in debug_counter_attributes.
void DebugCounter::addDebugCounterToSAI(const int num_attributes, const sai_attribute_t *debug_counter_attributes)
{
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

void DebugCounter::removeDebugCounterFromSAI()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("Removing debug counter '%s' from SAI", this->name.c_str());
    if (sai_debug_counter_api->remove_debug_counter(counter_id) != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to remove debug counter '%s'", this->name.c_str());
        throw std::runtime_error("Failed to remove debug counter");
    }
}
