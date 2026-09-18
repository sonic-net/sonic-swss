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
    void initialize(const std::deque<KeyOpFieldsValuesTuple> &entries);
    void process(const std::deque<KeyOpFieldsValuesTuple> &entries);

    const std::set<std::string> &getPendingInterfaces() const;
    void markResyncComplete();

private:
    enum class InterfaceMode
    {
        Ignored,
        Disabled,
        Enabled,
    };

    static InterfaceMode getInterfaceMode(const KeyOpFieldsValuesTuple &entry);

    std::set<std::string> m_enabledInterfaces;
    std::set<std::string> m_pendingInterfaces;
};

}

#endif
