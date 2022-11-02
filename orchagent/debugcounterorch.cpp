#include "debugcounterorch.h"
#include "portsorch.h"
#include "rediscommand.h"
#include "sai_serialize.h"
#include "schema.h"
#include "drop_counter.h"
#include <memory>
#include "observer.h"

using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;

static const unordered_map<string, CounterType> flex_counter_type_lookup = {
    { PORT_INGRESS_DROPS, CounterType::PORT_DEBUG },
    { PORT_EGRESS_DROPS, CounterType::PORT_DEBUG },
    { SWITCH_INGRESS_DROPS, CounterType::SWITCH_DEBUG },
    { SWITCH_EGRESS_DROPS, CounterType::SWITCH_DEBUG },
};

// Initializing DebugCounterOrch creates a group entry in FLEX_COUNTER_DB, so this
// object should only be initialized once.
DebugCounterOrch::DebugCounterOrch(DBConnector *db, const vector<string>& table_names, int poll_interval) :
        Orch(db, table_names),
        flex_counter_manager(DEBUG_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, poll_interval, true),
        m_stateDb(new DBConnector("STATE_DB", 0)),
        m_debugCapabilitiesTable(new Table(m_stateDb.get(), STATE_DEBUG_COUNTER_CAPABILITIES_NAME)),
        m_countersDb(new DBConnector("COUNTERS_DB", 0)),
        m_counterNameToPortStatMap(new Table(m_countersDb.get(), COUNTERS_DEBUG_NAME_PORT_STAT_MAP)),
        m_counterNameToSwitchStatMap(new Table(m_countersDb.get(), COUNTERS_DEBUG_NAME_SWITCH_STAT_MAP))
{
    SWSS_LOG_ENTER();
    publishDropCounterCapabilities();

    gPortsOrch->attach(this);
}

DebugCounterOrch::~DebugCounterOrch(void)
{
    SWSS_LOG_ENTER();
}

void DebugCounterOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    if (type == SUBJECT_TYPE_PORT_CHANGE) 
    {
        if (!cntx) 
        {
            SWSS_LOG_ERROR("cntx is NULL");
            return;
        }

        PortUpdate *update = static_cast<PortUpdate *>(cntx);
        Port &port = update->port;

        if (update->add) 
        {
            for (const auto& debug_counter: debug_counters)
            {
                DebugCounter *counter = debug_counter.second.get();
                auto counter_type = counter->getCounterType();
                auto counter_stat = counter->getDebugCounterSAIStat();
                auto flex_counter_type = getFlexCounterType(counter_type);
                if (flex_counter_type == CounterType::PORT_DEBUG)
                {
                    installDebugFlexCounters(counter_type, counter_stat, port.m_port_id);
                }
            }
        } 
        else 
        {
            for (const auto& debug_counter: debug_counters)
            {
                DebugCounter *counter = debug_counter.second.get();
                auto counter_type = counter->getCounterType();
                auto counter_stat = counter->getDebugCounterSAIStat();
                auto flex_counter_type = getFlexCounterType(counter_type);
                if (flex_counter_type == CounterType::PORT_DEBUG)
                {
                    uninstallDebugFlexCounters(counter_type, counter_stat, port.m_port_id);
                }
            }
        }
    }
}

// doTask processes updates from the consumer and modifies the state of the
// following components:
//     1) The ASIC, by creating, modifying, and deleting debug counters
//     2) syncd, by creating, modifying, and deleting flex counters to
//        keep track of the debug counters
//
// Updates can fail due to the following:
//    1) Malformed requests: if the update contains an unknown or unsupported
//       counter type or drop reason then the update will fail
//    2) SAI failures: if the SAI returns an error for any reason then the
//       update will fail
//  It is guaranteed that failed updates will not modify the state of the
//  system.
//
// In addition, updates are idempotent - repeating the same request any number
// of times will always result in the same external behavior.
void DebugCounterOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    // Currently we depend on 1) the switch and 2) the ports being ready
    // before we can set up the counters. If debug counters for other
    // object types are added we may need to update this dependency.
    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        std::vector<FieldValueTuple> values = kfvFieldsValues(t);

        auto table_name = consumer.getTableName();
        task_process_status task_status = task_process_status::task_ignore;
        if (table_name == CFG_DEBUG_COUNTER_TABLE_NAME)
        {
            if (op == SET_COMMAND)
            {
                try
                {
                    task_status = installDebugCounter(key, values);
                }
                catch (const std::runtime_error& e)
                {
                    SWSS_LOG_ERROR("Failed to create debug counter '%s'", key.c_str());
                    task_status = task_process_status::task_failed;
                }
            }
            else if (op == DEL_COMMAND)
            {
                try
                {
                    task_status = uninstallDebugCounter(key);
                }
                catch (const std::runtime_error& e)
                {
                    SWSS_LOG_ERROR("Failed to delete debug counter '%s'", key.c_str());
                    task_status = task_process_status::task_failed;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            }
        }
        else if (table_name == CFG_DEBUG_COUNTER_DROP_REASON_TABLE_NAME)
        {
            string counter_name, drop_reason;
            parseDropReasonUpdate(key, '|', &counter_name, &drop_reason);

            if (op == SET_COMMAND)
            {
                try
                {
                    task_status = addDropReason(counter_name, drop_reason);
                }
                catch (const std::runtime_error& e)
                {
                    SWSS_LOG_ERROR("Failed to add drop reason '%s' to counter '%s'", drop_reason.c_str(), counter_name.c_str());
                    task_status = task_process_status::task_failed;
                }
            }
            else if (op == DEL_COMMAND)
            {
                try
                {
                    task_status = removeDropReason(counter_name, drop_reason);
                }
                catch (const std::runtime_error& e)
                {
                    SWSS_LOG_ERROR("Failed to remove drop reason '%s' from counter '%s'", drop_reason.c_str(), counter_name.c_str());
                    task_status = task_process_status::task_failed;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("Received update from unknown table '%s'", table_name.c_str());
        }

        switch (task_status)
        {
            case task_process_status::task_success:
                consumer.m_toSync.erase(it++);
                break;
            case task_process_status::task_ignore:
                SWSS_LOG_WARN("Debug counters '%s' task ignored", op.c_str());
                consumer.m_toSync.erase(it++);
                break;
            case task_process_status::task_need_retry:
                SWSS_LOG_NOTICE("Failed to process debug counters '%s' task, retrying", op.c_str());
                ++it;
                break;
            case task_process_status::task_failed:
                SWSS_LOG_ERROR("Failed to process debug counters '%s' task, error(s) occurred during execution", op.c_str());
                consumer.m_toSync.erase(it++);
                break;
            default:
                SWSS_LOG_ERROR("Invalid task status %d", task_status);
                consumer.m_toSync.erase(it++);
                break;
        }
    }
}

// Debug Capability Reporting Functions START HERE -------------------------------------------------

// publishDropCounterCapabilities queries the SAI for available drop counter
// capabilities on this device and publishes the information to the
// DROP_COUNTER_CAPABILITIES table in STATE_DB.
void DebugCounterOrch::publishDropCounterCapabilities()
{
    supported_ingress_drop_reasons = DropCounter::getSupportedDropReasons(SAI_DEBUG_COUNTER_ATTR_IN_DROP_REASON_LIST);
    supported_egress_drop_reasons  = DropCounter::getSupportedDropReasons(SAI_DEBUG_COUNTER_ATTR_OUT_DROP_REASON_LIST);
    supported_counter_types        = DropCounter::getSupportedCounterTypes();

    string ingress_drop_reason_str = DropCounter::serializeSupportedDropReasons(supported_ingress_drop_reasons);
    string egress_drop_reason_str = DropCounter::serializeSupportedDropReasons(supported_egress_drop_reasons);

    for (auto const &counter_type : DebugCounter::getDebugCounterTypeLookup())
    {
        string drop_reasons;

        if (!supported_counter_types.count(counter_type.first))
        {
            continue;
        }

        if (counter_type.first == PORT_INGRESS_DROPS || counter_type.first == SWITCH_INGRESS_DROPS)
        {
            drop_reasons = ingress_drop_reason_str;
        }
        else
        {
            drop_reasons = egress_drop_reason_str;
        }

        // Don't bother publishing counters that have no drop reasons
        if (drop_reasons.empty())
        {
            continue;
        }

        string num_counters = std::to_string(DropCounter::getSupportedDebugCounterAmounts(counter_type.second));

        // Don't bother publishing counters that aren't available.
        if (num_counters == "0")
        {
            continue;
        }

        vector<FieldValueTuple> fieldValues;
        fieldValues.push_back(FieldValueTuple("count", num_counters));
        fieldValues.push_back(FieldValueTuple("reasons", drop_reasons));

        SWSS_LOG_DEBUG("Setting '%s' capabilities to count='%s', reasons='%s'", counter_type.first.c_str(), num_counters.c_str(), drop_reasons.c_str());
        m_debugCapabilitiesTable->set(counter_type.first, fieldValues);
    }
}

// doTask Handler Functions START HERE -------------------------------------------------------------

// Note that this function cannot be used to re-initialize a counter. To create a counter
// with the same name but different attributes (e.g. type, drop reasons, etc.) you will need
// to delete the original counter first or use a function like addDropReason, if available.
task_process_status DebugCounterOrch::installDebugCounter(const string& counter_name, const vector<FieldValueTuple>& attributes)
{
    SWSS_LOG_ENTER();

    if (debug_counters.find(counter_name) != debug_counters.end())
    {
        SWSS_LOG_DEBUG("Debug counter '%s' already exists", counter_name.c_str());
        return task_process_status::task_success;
    }

    // NOTE: this method currently assumes that all counters are drop counters.
    // If you are adding support for a non-drop counter than it may make sense
    // to either: a) dispatch to different handlers in doTask or b) dispatch to
    // different helper methods in this method.

    string counter_type = getDebugCounterType(attributes);

    if (supported_counter_types.find(counter_type) == supported_counter_types.end())
    {
        SWSS_LOG_ERROR("Specified counter type '%s' is not supported.", counter_type.c_str());
        return task_process_status::task_failed;
    }

    addFreeCounter(counter_name, counter_type);
    reconcileFreeDropCounters(counter_name);

    SWSS_LOG_NOTICE("Successfully created drop counter %s", counter_name.c_str());
    return task_process_status::task_success;
}

task_process_status DebugCounterOrch::uninstallDebugCounter(const string& counter_name)
{
    SWSS_LOG_ENTER();

    auto it = debug_counters.find(counter_name);
    if (it == debug_counters.end())
    {
        if (free_drop_counters.find(counter_name) != free_drop_counters.end())
        {
            deleteFreeCounter(counter_name);
        }
        else
        {
            SWSS_LOG_ERROR("Debug counter %s does not exist", counter_name.c_str());
        }

        return task_process_status::task_ignore;
    }

    DebugCounter *counter = it->second.get();
    string counter_type = counter->getCounterType();
    string counter_stat = counter->getDebugCounterSAIStat();

    debug_counters.erase(it);
    uninstallDebugFlexCounters(counter_type, counter_stat);

    if (counter_type == PORT_INGRESS_DROPS || counter_type == PORT_EGRESS_DROPS)
    {
        m_counterNameToPortStatMap->hdel("", counter_name);
    }
    else
    {
        m_counterNameToSwitchStatMap->hdel("", counter_name);
    }

    SWSS_LOG_NOTICE("Successfully deleted drop counter %s", counter_name.c_str());
    return task_process_status::task_success;
}

task_process_status DebugCounterOrch::addDropReason(const string& counter_name, const string& drop_reason)
{
    SWSS_LOG_ENTER();

    if (!isDropReasonValid(drop_reason))
    {
        SWSS_LOG_ERROR("Specified drop reason '%s' is invalid.", drop_reason.c_str());
        return task_process_status::task_failed;
    }

    if (supported_ingress_drop_reasons.find(drop_reason) == supported_ingress_drop_reasons.end() &&
        supported_egress_drop_reasons.find(drop_reason) == supported_egress_drop_reasons.end())
    {
        SWSS_LOG_ERROR("Specified drop reason '%s' is not supported.", drop_reason.c_str());
        return task_process_status::task_failed;
    }

    auto it = debug_counters.find(counter_name);
    if (it == debug_counters.end())
    {
        // In order to gracefully handle the case where the drop reason updates
        // are received before the create counter update, we keep track of reasons
        // we've seen in the free_drop_reasons table.
        addFreeDropReason(counter_name, drop_reason);
        SWSS_LOG_NOTICE("Added drop reason %s to drop counter %s", drop_reason.c_str(), counter_name.c_str());

        reconcileFreeDropCounters(counter_name);
        return task_process_status::task_success;
    }

    DropCounter *counter = dynamic_cast<DropCounter*>(it->second.get());
    counter->addDropReason(drop_reason);

    SWSS_LOG_NOTICE("Added drop reason %s to drop counter %s", drop_reason.c_str(), counter_name.c_str());
    return task_process_status::task_success;
}

// A drop counter must always contain at least one drop reason, so this function
// will do nothing if you attempt to remove the last drop reason.
task_process_status DebugCounterOrch::removeDropReason(const string& counter_name, const string& drop_reason)
{
    SWSS_LOG_ENTER();

    if (!isDropReasonValid(drop_reason))
    {
        return task_failed;
    }

    auto it = debug_counters.find(counter_name);
    if (it == debug_counters.end())
    {
        deleteFreeDropReason(counter_name, drop_reason);
        return task_success;
    }

    DropCounter *counter = dynamic_cast<DropCounter*>(it->second.get());
    const unordered_set<string>& drop_reasons = counter->getDropReasons();

    if (drop_reasons.size() <= 1)
    {
        SWSS_LOG_WARN("Attempted to remove all drop reasons from counter '%s'", counter_name.c_str());
        return task_ignore;
    }

    counter->removeDropReason(drop_reason);

    SWSS_LOG_NOTICE("Removed drop reason %s from drop counter %s", drop_reason.c_str(), counter_name.c_str());
    return task_success;
}

// Free Table Management Functions START HERE ------------------------------------------------------

// Note that entries will remain in the table until at least one drop reason is added to the counter.
void DebugCounterOrch::addFreeCounter(const string& counter_name, const string& counter_type)
{
    SWSS_LOG_ENTER();

    if (free_drop_counters.find(counter_name) != free_drop_counters.end())
    {
        SWSS_LOG_DEBUG("Debug counter '%s' is in free counter table", counter_name.c_str());
        return;
    }

    SWSS_LOG_DEBUG("Adding debug counter '%s' to free counter table", counter_name.c_str());
    free_drop_counters.emplace(counter_name, counter_type);
}

void DebugCounterOrch::deleteFreeCounter(const string& counter_name)
{
    SWSS_LOG_ENTER();

    if (free_drop_counters.find(counter_name) == free_drop_counters.end())
    {
        SWSS_LOG_ERROR("Debug counter %s does not exist", counter_name.c_str());
        return;
    }

    SWSS_LOG_DEBUG("Removing debug counter '%s' from free counter table", counter_name.c_str());
    free_drop_counters.erase(counter_name);
}

// Note that entries will remain in the table until a drop counter is added.
void DebugCounterOrch::addFreeDropReason(const string& counter_name, const string& drop_reason)
{
    SWSS_LOG_ENTER();

    auto reasons_it = free_drop_reasons.find(counter_name);

    if (reasons_it == free_drop_reasons.end())
    {
        SWSS_LOG_DEBUG("Creating free drop reason table for counter '%s'", counter_name.c_str());
        unordered_set<string> new_reasons = { drop_reason };
        free_drop_reasons.emplace(make_pair(counter_name, new_reasons));
    }
    else
    {
        SWSS_LOG_DEBUG("Adding additional drop reasons to free drop reason table for counter '%s'", counter_name.c_str());
        reasons_it->second.emplace(drop_reason);
    }
}

void DebugCounterOrch::deleteFreeDropReason(const string& counter_name, const string& drop_reason)
{
    SWSS_LOG_ENTER();

    auto reasons_it = free_drop_reasons.find(counter_name);

    if (reasons_it == free_drop_reasons.end()) {
        SWSS_LOG_DEBUG("Attempted to remove drop reason '%s' from counter '%s' that does not exist", drop_reason.c_str(), counter_name.c_str());
        return;
    }

    SWSS_LOG_DEBUG("Removing free drop reason from counter '%s'", counter_name.c_str());
    reasons_it->second.erase(drop_reason);

    if (reasons_it->second.empty()) {
        free_drop_reasons.erase(reasons_it);
    }
}

void DebugCounterOrch::reconcileFreeDropCounters(const string& counter_name)
{
    SWSS_LOG_ENTER();

    auto counter_it = free_drop_counters.find(counter_name);
    auto reasons_it = free_drop_reasons.find(counter_name);

    if (counter_it != free_drop_counters.end() && reasons_it != free_drop_reasons.end())
    {
        SWSS_LOG_DEBUG("Found counter '%s' and drop reasons, creating the counter", counter_name.c_str());
        createDropCounter(counter_name, counter_it->second, reasons_it->second);
        free_drop_counters.erase(counter_it);
        free_drop_reasons.erase(reasons_it);
        SWSS_LOG_NOTICE("Successfully matched drop reasons to counter %s", counter_name.c_str());
    }
}

// Flex Counter Management Functions START HERE ----------------------------------------------------

CounterType DebugCounterOrch::getFlexCounterType(const string& counter_type)
{
    SWSS_LOG_ENTER();

    auto flex_counter_type_it = flex_counter_type_lookup.find(counter_type);
    if (flex_counter_type_it == flex_counter_type_lookup.end())
    {
        SWSS_LOG_ERROR("Flex counter type '%s' not found", counter_type.c_str());
        throw runtime_error("Flex counter type not found");
    }
    return flex_counter_type_it->second;
}

void DebugCounterOrch::installDebugFlexCounters(const string& counter_type,
                                                const string& counter_stat,
                                                sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();
    CounterType flex_counter_type = getFlexCounterType(counter_type);

    if (flex_counter_type == CounterType::SWITCH_DEBUG)
    {
        flex_counter_manager.addFlexCounterStat(gSwitchId, flex_counter_type, counter_stat);
    }
    else if (flex_counter_type == CounterType::PORT_DEBUG)
    {
        for (auto const &curr : gPortsOrch->getAllPorts())
        {
            if (port_id != SAI_NULL_OBJECT_ID)
            {
                if (curr.second.m_port_id != port_id)
                {
                    continue;
                }
            }

            if (curr.second.m_type != Port::Type::PHY)
            {
                continue;
            }

            flex_counter_manager.addFlexCounterStat(
                    curr.second.m_port_id,
                    flex_counter_type,
                    counter_stat);
        }
    }
}

void DebugCounterOrch::uninstallDebugFlexCounters(const string& counter_type,
                                                  const string& counter_stat,
                                                  sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();
    CounterType flex_counter_type = getFlexCounterType(counter_type);

    if (flex_counter_type == CounterType::SWITCH_DEBUG)
    {
        flex_counter_manager.removeFlexCounterStat(gSwitchId, flex_counter_type, counter_stat);
    }
    else if (flex_counter_type == CounterType::PORT_DEBUG)
    {
        for (auto const &curr : gPortsOrch->getAllPorts())
        {
            if (port_id != SAI_NULL_OBJECT_ID)
            {
                if (curr.second.m_port_id != port_id)
                {
                    continue;
                }
            }

            if (curr.second.m_type != Port::Type::PHY)
            {
                continue;
            }

            flex_counter_manager.removeFlexCounterStat(
                curr.second.m_port_id,
                flex_counter_type,
                counter_stat);
        }
    }
}

// Debug Counter Initialization Helper Functions START HERE ----------------------------------------

// NOTE: At this point COUNTER_TYPE is the only field from CONFIG_DB that we care about. In the future
// if other fields become relevant it may make sense to extend this method and return a struct with the
// relevant fields.
std::string DebugCounterOrch::getDebugCounterType(const vector<FieldValueTuple>& values) const
{
    SWSS_LOG_ENTER();

    std::string counter_type;
    for (auto attr : values)
    {
        std::string attr_name = fvField(attr);
        auto supported_debug_counter_attributes = DebugCounter::getSupportedDebugCounterAttributes();
        auto attr_name_it = supported_debug_counter_attributes.find(attr_name);
        if (attr_name_it == supported_debug_counter_attributes.end())
        {
            SWSS_LOG_ERROR("Unknown debug counter attribute '%s'", attr_name.c_str());
            continue;
        }

        std::string attr_value = fvValue(attr);
        if (attr_name == "type")
        {
            auto debug_counter_type_lookup = DebugCounter::getDebugCounterTypeLookup();
            auto counter_type_it = debug_counter_type_lookup.find(attr_value);
            if (counter_type_it == debug_counter_type_lookup.end())
            {
                SWSS_LOG_ERROR("Debug counter type '%s' does not exist", attr_value.c_str());
                throw std::runtime_error("Failed to initialize debug counter");
            }

            counter_type = counter_type_it->first;
        }
    }

    return counter_type;
}

// createDropCounter creates a new drop counter in the SAI and installs a
// flex counter to poll the counter data.
//
// If SAI initialization fails or flex counter installation fails then this
// method will throw an exception.
void DebugCounterOrch::createDropCounter(const string& counter_name, const string& counter_type, const unordered_set<string>& drop_reasons)
{
    auto counter = std::unique_ptr<DropCounter>(new DropCounter(counter_name, counter_type, drop_reasons));
    std::string counter_stat = counter->getDebugCounterSAIStat();
    debug_counters.emplace(counter_name, std::move(counter));
    installDebugFlexCounters(counter_type, counter_stat);

    if (counter_type == PORT_INGRESS_DROPS || counter_type == PORT_EGRESS_DROPS)
    {
        m_counterNameToPortStatMap->set("", { FieldValueTuple(counter_name, counter_stat) });
    }
    else
    {
        m_counterNameToSwitchStatMap->set("", { FieldValueTuple(counter_name, counter_stat) });
    }
}

// Debug Counter Configuration Helper Functions START HERE -----------------------------------------

// parseDropReasonUpdate takes a key from CONFIG_DB and returns the 1) the counter name being targeted and
// 2) the drop reason to be added or removed via output parameters.
void DebugCounterOrch::parseDropReasonUpdate(const string& key, const char delimeter, string *counter_name, string *drop_reason) const
{
    size_t counter_end = key.find(delimeter);
    *counter_name = key.substr(0, counter_end);
    SWSS_LOG_DEBUG("DEBUG_COUNTER COUNTER NAME = %s (%d, %zd)", counter_name->c_str(), 0, counter_end);
    *drop_reason = key.substr(counter_end + 1);
    SWSS_LOG_DEBUG("DEBUG_COUNTER RULE NAME = %s (%zd, %zd)", drop_reason->c_str(), counter_end + 1, key.length());
}

bool DebugCounterOrch::isDropReasonValid(const string& drop_reason) const
{
    SWSS_LOG_ENTER();

    if (!DropCounter::isIngressDropReasonValid(drop_reason) &&
        !DropCounter::isEgressDropReasonValid(drop_reason))
    {
        SWSS_LOG_ERROR("Drop reason %s not found", drop_reason.c_str());
        return false;
    }

    return true;
}



