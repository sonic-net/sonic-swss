#include "bpprotorch.h"
#include "portsorch.h"
#include <stdexcept>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <limits>
#include <set>
#include <syslog.h>

#include "logger.h"

using namespace swss;

namespace
{
std::vector<TableConnector> protectionTables(DBConnector *config, DBConnector *app)
{
    std::vector<TableConnector> tables;
    tables.emplace_back(config, CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    if (app)
        tables.emplace_back(app, APP_BP_PROTECTION_BINDING_TABLE_NAME);
    return tables;
}

std::atomic<Logger::Priority> logPriority{Logger::SWSS_NOTICE};
std::atomic<Logger::Output> logOutput{Logger::SWSS_SYSLOG};


bool validPortChannelName(const std::string &name)
{
    const std::string prefix = "PortChannel";
    return name.compare(0, prefix.size(), prefix) == 0 &&
        name.size() > prefix.size() && name.size() <= prefix.size() + 4 &&
        std::all_of(name.begin() + prefix.size(), name.end(),
            [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool parseTimer(const std::string &value, uint32_t &timer)
{
    if (value.empty())
        return false;

    uint32_t parsed = 0;
    for (char ch : value)
    {
        if (ch < '0' || ch > '9')
            return false;
        const uint32_t digit = ch - '0';
        if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    timer = parsed;
    return true;
}

bool parsePolicy(const std::vector<FieldValueTuple> &fields, BpProtEntry &entry)
{
    std::set<std::string> seen;
    for (const auto &field : fields)
    {
        const auto &name = fvField(field);
        const auto &value = fvValue(field);
        if (!seen.insert(name).second)
            return false;

        // ConfigDBConnector encodes an empty JSON object as NULL=NULL.
        // It may remain alongside explicit fields after an incremental SET.
        if (name == "NULL")
        {
            if (value != "NULL")
                return false;
            continue;
        }
        if (name == "protection_mode")
        {
            if (value != "SOFTWARE")
                return false;
        }
        else if (name == "admin_mode")
        {
            if (value == "AUTO")
                entry.adminMode = BpProtAdminMode::AUTO;
            else if (value == "PRIMARY")
                entry.adminMode = BpProtAdminMode::PRIMARY;
            else if (value == "PROTECTION")
                entry.adminMode = BpProtAdminMode::PROTECTION;
            else
                return false;
        }
        else if (name == "revertive")
        {
            if (value != "true" && value != "false")
                return false;
            entry.revertive = value == "true";
        }
        else if (name == "wait_to_restore_time_ms")
        {
            if (!parseTimer(value, entry.waitToRestoreTimeMs))
                return false;
        }
        else
        {
            return false;
        }
    }
    return true;
}
}

void BpProtLog::initialize()
{
    Logger::linkToDbWithOutput("bpProtOrch", setPriority, "NOTICE", setOutput, "SYSLOG");
}

void BpProtLog::setPriority(const std::string &, const std::string &value)
{
    const auto found = Logger::priorityStringMap.find(value);
    logPriority = found == Logger::priorityStringMap.end() ? Logger::SWSS_NOTICE : found->second;
}

void BpProtLog::setOutput(const std::string &, const std::string &value)
{
    const auto found = Logger::outputStringMap.find(value);
    logOutput = found == Logger::outputStringMap.end() ? Logger::SWSS_SYSLOG : found->second;
}

void BpProtLog::write(Logger::Priority priority, const char *format, ...)
{
    if (priority > logPriority.load())
        return;
    va_list args;
    va_start(args, format);
    const auto output = logOutput.load();
    if (output == Logger::SWSS_SYSLOG)
        vsyslog(static_cast<int>(priority), format, args);
    else
    {
        FILE *stream = output == Logger::SWSS_STDOUT ? stdout : stderr;
        vfprintf(stream, format, args);
        fputc('\n', stream);
    }
    va_end(args);
}

BpProtOrch::BpProtOrch(DBConnector *configDb, DBConnector *stateDb) :
    BpProtOrch(configDb, stateDb, nullptr, nullptr, nullptr)
{
}

BpProtOrch::BpProtOrch(DBConnector *configDb, DBConnector *stateDb,
    DBConnector *appDb, PortsOrch *ports, L2NhgOrch *groups) :
    Orch(protectionTables(configDb, appDb)),
    m_ports(ports), m_groups(groups),
    m_configTable(configDb, CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME),
    m_stateTable(stateDb, STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME)
{
    BP_PROT_LOG_ENTER();
    if (appDb)
    {
        if (!m_ports || !m_groups)
            throw std::invalid_argument("BpProtOrch runtime requires PortsOrch and L2NhgOrch");
        m_bindingTable = std::make_unique<Table>(appDb, APP_BP_PROTECTION_BINDING_TABLE_NAME);
        initializeFsmTimer();
        m_ports->attach(this);
        m_groups->attach(this);
        m_observersAttached = true;
    }
}

const BpProtEntry *BpProtOrch::getEntry(const std::string &ifname) const
{
    const auto entry = m_entries.find(ifname);
    return entry == m_entries.end() ? nullptr : &entry->second;
}

void BpProtOrch::publishConfig(const std::string &ifname, const BpProtEntry &entry)
{
    BP_PROT_LOG_ENTER();
    const char *adminMode = "UNKNOWN";
    switch (entry.adminMode)
    {
    case BpProtAdminMode::AUTO:
        adminMode = "AUTO";
        break;
    case BpProtAdminMode::PRIMARY:
        adminMode = "PRIMARY";
        break;
    case BpProtAdminMode::PROTECTION:
        adminMode = "PROTECTION";
        break;
    }
    // CONFIG acceptance alone cannot certify a SAI path or STEADY state.
    const std::vector<FieldValueTuple> fields = {
        {"protection_type", "sw_protection"}, {"admin_mode", adminMode}
    };
    // Runtime publication writes the complete HLD row and preserves only a
    // valid observation of the current bridge-port attachment.
    m_stateTable.set(ifname, fields);
    publishRuntime(ifname, "INCONSISTENT", "config_set");
    BP_PROT_LOG(SWSS_NOTICE, "event=config_set ifname=%s config_admin_mode=%s reconciliation_state=INCONSISTENT",
        ifname.c_str(), adminMode);
    std::vector<FieldValueTuple> published;
    m_stateTable.get(ifname, published);
    for (const auto &field : published)
        BP_PROT_LOG(SWSS_INFO, "event=state_publish ifname=%s field=%s value=%s",
            ifname.c_str(), fvField(field).c_str(), fvValue(field).c_str());
}

void BpProtOrch::doTask(Consumer &consumer)
{
    BP_PROT_LOG_ENTER();
    if (consumer.getTableName() == APP_BP_PROTECTION_BINDING_TABLE_NAME)
    {
        consumeBinding(consumer);
        return;
    }
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        const auto &task = it->second;
        const auto &ifname = kfvKey(task);
        const auto &op = kfvOp(task);
        if (op == DEL_COMMAND)
        {
            auto attachment = m_attachments.find(ifname);
            // Explicit CONFIG deletion may retry a previously failed cleanup once.
            if (attachment != m_attachments.end()) attachment->second.cleanupBlocked = false;
            m_entries.erase(ifname);
            m_stateTable.del(ifname);
            BP_PROT_LOG(SWSS_NOTICE, "event=config_del ifname=%s state_deleted=true", ifname.c_str());
        }
        else if (op == SET_COMMAND)
        {
            // Consumer coalescing merges SET fields, which can retain a field
            // removed from Redis. Read the authoritative row before parsing.
            std::vector<FieldValueTuple> fields;
            if (!m_configTable.get(ifname, fields))
            {
                m_entries.erase(ifname);
                m_stateTable.del(ifname);
                BP_PROT_LOG(SWSS_NOTICE, "event=stale_set ifname=%s state_deleted=true", ifname.c_str());
                if (m_bindingTable)
                    reconcile(ifname);
                updateProtectionIntent(ifname);
                it = consumer.m_toSync.erase(it);
                continue;
            }
            // Start from defaults so field deletion clears cached policy.
            BpProtEntry candidate;
            if (validPortChannelName(ifname) && parsePolicy(fields, candidate))
            {
                m_entries[ifname] = candidate;
                publishConfig(ifname, candidate);
                if (m_bindingTable)
                {
                    auto &attachment = m_attachments[ifname];
                    // A validated protection CONFIG SET is the explicit reapplication boundary.
                    if (attachment.blocked)
                    {
                        attachment.wtrEligible = false; attachment.wtrCompleted = false;
                    }
                    attachment.blocked = false; attachment.cleanupBlocked = false;
                    attachment.partialFailure = false;
                    attachment.terminalOperation.clear(); attachment.terminalAttributes.clear();
                    attachment.terminalStatus = SAI_STATUS_SUCCESS;
                    attachment.retryEvent = true;
                }
            }
            else
            {
                BP_PROT_LOG(SWSS_ERROR, "event=config_reject ifname=%s reason=invalid_policy_or_interface", ifname.c_str());
            }
        }
        else
        {
            BP_PROT_LOG(SWSS_ERROR, "event=config_reject ifname=%s reason=unsupported_operation op=%s", ifname.c_str(), op.c_str());
        }
        if (m_bindingTable)
            reconcile(ifname);
        updateProtectionIntent(ifname);
        it = consumer.m_toSync.erase(it);
    }
}
