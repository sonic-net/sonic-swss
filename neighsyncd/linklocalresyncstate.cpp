#include "linklocalresyncstate.h"

#include "schema.h"

namespace swss
{

LinkLocalResyncState::InterfaceMode LinkLocalResyncState::getInterfaceMode(
    const KeyOpFieldsValuesTuple &entry)
{
    if (kfvKey(entry).find('|') != std::string::npos)
    {
        return InterfaceMode::Ignored;
    }

    if (kfvOp(entry) == DEL_COMMAND)
    {
        return InterfaceMode::Disabled;
    }

    if (kfvOp(entry) != SET_COMMAND)
    {
        return InterfaceMode::Ignored;
    }

    // SubscriberStateTable returns the complete current row for SET events.
    for (const auto &field : kfvFieldsValues(entry))
    {
        if (fvField(field) == "ipv6_use_link_local_only" && fvValue(field) == "enable")
        {
            return InterfaceMode::Enabled;
        }
    }

    return InterfaceMode::Disabled;
}

void LinkLocalResyncState::initialize(const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    for (const auto &entry : entries)
    {
        switch (getInterfaceMode(entry))
        {
        case InterfaceMode::Enabled:
            m_enabledInterfaces.insert(kfvKey(entry));
            break;
        case InterfaceMode::Disabled:
            m_enabledInterfaces.erase(kfvKey(entry));
            break;
        case InterfaceMode::Ignored:
            break;
        }
    }
}

void LinkLocalResyncState::process(const std::deque<KeyOpFieldsValuesTuple> &entries)
{
    for (const auto &entry : entries)
    {
        switch (getInterfaceMode(entry))
        {
        case InterfaceMode::Enabled:
            if (m_enabledInterfaces.insert(kfvKey(entry)).second)
            {
                m_pendingInterfaces.insert(kfvKey(entry));
            }
            break;
        case InterfaceMode::Disabled:
            m_enabledInterfaces.erase(kfvKey(entry));
            m_pendingInterfaces.erase(kfvKey(entry));
            break;
        case InterfaceMode::Ignored:
            break;
        }
    }
}

const std::set<std::string> &LinkLocalResyncState::getPendingInterfaces() const
{
    return m_pendingInterfaces;
}

void LinkLocalResyncState::markResyncComplete()
{
    m_pendingInterfaces.clear();
}

}
