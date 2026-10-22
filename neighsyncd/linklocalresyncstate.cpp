#include "linklocalresyncstate.h"

#include "schema.h"

namespace swss
{

LinkLocalResyncState::InterfaceUpdate LinkLocalResyncState::getInterfaceUpdate(
    const KeyOpFieldsValuesTuple &entry)
{
    if (kfvKey(entry).find('|') != std::string::npos)
    {
        return InterfaceUpdate::Ignore;
    }

    if (kfvOp(entry) == DEL_COMMAND)
    {
        return InterfaceUpdate::Disable;
    }

    if (kfvOp(entry) != SET_COMMAND)
    {
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

void LinkLocalResyncState::initialize(const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    for (const auto &entry : entries)
    {
        switch (getInterfaceUpdate(entry))
        {
        case InterfaceUpdate::Enable:
            m_enabledInterfaces.insert(kfvKey(entry));
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
    const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    InterfaceTransitions transitions;

    for (const auto &entry : entries)
    {
        const auto &interface = kfvKey(entry);
        switch (getInterfaceUpdate(entry))
        {
        case InterfaceUpdate::Enable:
            if (m_enabledInterfaces.insert(interface).second)
            {
                m_pendingInterfaces.insert(interface);
                transitions.enabled.insert(interface);
            }
            break;
        case InterfaceUpdate::Disable:
            m_enabledInterfaces.erase(interface);
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
    m_pendingInterfaces.erase(interface);
}

}
