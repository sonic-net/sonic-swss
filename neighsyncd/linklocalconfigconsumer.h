#ifndef SWSS_LINKLOCALCONFIGCONSUMER_H
#define SWSS_LINKLOCALCONFIGCONSUMER_H

#include "linklocalresyncstate.h"

namespace swss
{

template <typename ConfigTable>
void initializeLinkLocalConfigTable(ConfigTable &table, LinkLocalResyncState &state)
{
    std::deque<KeyOpFieldsValuesTuple> entries;
    table.pops(entries);
    state.initialize(table.getTableName(), entries);
}

template <typename ConfigTable>
LinkLocalResyncState::InterfaceTransitions processLinkLocalConfigTable(
    ConfigTable &table, LinkLocalResyncState &state)
{
    std::deque<KeyOpFieldsValuesTuple> entries;
    table.pops(entries);
    return state.process(table.getTableName(), entries);
}

}

#endif
