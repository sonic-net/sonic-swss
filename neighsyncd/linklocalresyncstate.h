#ifndef SWSS_LINKLOCALRESYNCSTATE_H
#define SWSS_LINKLOCALRESYNCSTATE_H

#include <deque>
#include <set>
#include <string>

#include "table.h"

namespace swss
{

class LinkLocalResyncState
{
public:
    struct InterfaceTransitions
    {
        std::set<std::string> enabled;
        std::set<std::string> disabled;
    };

    void initialize(const std::deque<KeyOpFieldsValuesTuple> &entries);
    InterfaceTransitions process(const std::deque<KeyOpFieldsValuesTuple> &entries);

    const std::set<std::string> &getPendingInterfaces() const;
    void markResyncComplete(const std::string &interface);

private:
    enum class InterfaceUpdate
    {
        Ignore,
        Disable,
        Enable,
    };

    static InterfaceUpdate getInterfaceUpdate(const KeyOpFieldsValuesTuple &entry);

    std::set<std::string> m_enabledInterfaces;
    std::set<std::string> m_pendingInterfaces;
};

}

#endif
