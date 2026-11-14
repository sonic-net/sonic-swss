#include "linklocalresyncstate.h"
#include "linklocalconfig.h"
#include "logger.h"

#include "schema.h"

namespace swss
{

LinkLocalResyncState::InterfaceUpdate LinkLocalResyncState::getInterfaceUpdate(
    const std::string &tableName, const KeyOpFieldsValuesTuple &entry)
{
    if (kfvKey(entry).find('|') != std::string::npos)
    {
        SWSS_LOG_DEBUG("Ignoring link-local address row %s|%s", tableName.c_str(), kfvKey(entry).c_str());
        return InterfaceUpdate::Ignore;
    }

    const auto expectedTable = getLinkLocalInterfaceTable(kfvKey(entry));
    if (expectedTable.empty() || tableName != expectedTable)
    {
        SWSS_LOG_DEBUG("Ignoring link-local update from %s for %s (expected table: %s)",
                       tableName.c_str(), kfvKey(entry).c_str(), expectedTable.c_str());
        return InterfaceUpdate::Ignore;
    }

    if (kfvOp(entry) == DEL_COMMAND)
    {
        return InterfaceUpdate::Disable;
    }

    if (kfvOp(entry) != SET_COMMAND)
    {
        SWSS_LOG_DEBUG("Ignoring link-local operation %s from %s for %s",
                       kfvOp(entry).c_str(), tableName.c_str(), kfvKey(entry).c_str());
        return InterfaceUpdate::Ignore;
    }

    // SubscriberStateTable returns the complete current row for SET events.
    for (const auto &field : kfvFieldsValues(entry))
    {
        if (fvField(field) == "ipv6_use_link_local_only" && fvValue(field) == "enable")
        {
            return InterfaceUpdate::Enable;
        }
    }

    return InterfaceUpdate::Disable;
}

void LinkLocalResyncState::initialize(
    const std::string &tableName, const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    for (const auto &entry : entries)
    {
        switch (getInterfaceUpdate(tableName, entry))
        {
        case InterfaceUpdate::Enable:
            if (m_enabledInterfaces.insert(kfvKey(entry)).second)
            {
                SWSS_LOG_INFO("Initialized link-local mode for %s from %s",
                              kfvKey(entry).c_str(), tableName.c_str());
            }
            break;
        case InterfaceUpdate::Disable:
            m_enabledInterfaces.erase(kfvKey(entry));
            break;
        case InterfaceUpdate::Ignore:
            break;
        }
    }
}

LinkLocalResyncState::InterfaceTransitions LinkLocalResyncState::process(
    const std::string &tableName, const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    InterfaceTransitions transitions;

    for (const auto &entry : entries)
    {
        const auto &interface = kfvKey(entry);
        switch (getInterfaceUpdate(tableName, entry))
        {
        case InterfaceUpdate::Enable:
            if (m_enabledInterfaces.insert(interface).second)
            {
                m_pendingInterfaces.insert(interface);
                transitions.enabled.insert(interface);
                SWSS_LOG_INFO("Link-local mode enabled for %s from %s; neighbor replay pending",
                              interface.c_str(), tableName.c_str());
            }
            else
            {
                SWSS_LOG_DEBUG("Link-local mode already enabled for %s from %s",
                               interface.c_str(), tableName.c_str());
            }
            break;
        case InterfaceUpdate::Disable:
            if (m_enabledInterfaces.erase(interface) != 0)
            {
                SWSS_LOG_INFO("Link-local mode disabled for %s from %s",
                              interface.c_str(), tableName.c_str());
            }
            m_pendingInterfaces.erase(interface);
            transitions.disabled.insert(interface);
            break;
        case InterfaceUpdate::Ignore:
            break;
        }
    }

    return transitions;
}

const std::set<std::string> &LinkLocalResyncState::getPendingInterfaces() const
{
    return m_pendingInterfaces;
}

void LinkLocalResyncState::markResyncComplete(const std::string &interface)
{
    if (m_pendingInterfaces.erase(interface) != 0)
    {
        SWSS_LOG_DEBUG("Link-local neighbor replay completed for %s", interface.c_str());
    }
}

}
