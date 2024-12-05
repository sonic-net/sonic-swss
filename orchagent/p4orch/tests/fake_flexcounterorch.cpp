#include "copporch.h"
#include "flexcounterorch.h"

FlexCounterOrch::FlexCounterOrch(swss::DBConnector *db, std::vector<std::string> &tableNames)
    : Orch(db, tableNames),
      m_bufferQueueConfigTable(db, CFG_BUFFER_QUEUE_TABLE_NAME), m_bufferPgConfigTable(db, CFG_BUFFER_PG_TABLE_NAME),
      m_deviceMetadataConfigTable(db, CFG_DEVICE_METADATA_TABLE_NAME)
{
}

FlexCounterOrch::~FlexCounterOrch(void)
{
}

void FlexCounterOrch::doTask(Consumer &consumer)
{
}

void FlexCounterOrch::doTask(SelectableTimer &timer)
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