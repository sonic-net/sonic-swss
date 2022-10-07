#include "copporch.h"
#include "flexcounterorch.h"

FlexCounterOrch::FlexCounterOrch(swss::DBConnector *db, std::vector<std::string> &tableNames)
    : Orch(db, tableNames), m_flexCounterConfigTable(db, CFG_FLEX_COUNTER_TABLE_NAME)
{
}

FlexCounterOrch::~FlexCounterOrch(void)
{
}

void FlexCounterOrch::doTask(Consumer &consumer)
{
}

bool FlexCounterOrch::getPortCountersState() const
{
    return true;
}

bool FlexCounterOrch::getPortBufferDropCountersState() const
{
    return true;
}

bool FlexCounterOrch::bake()
{
    return true;
}