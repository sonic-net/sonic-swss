extern "C"
{
#include "sai.h"
}

#include "fabricportsorch.h"

#define FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP         "FABRIC_PORT_STAT_COUNTER"
#define FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS   10000
#define FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP        "FABRIC_QUEUE_STAT_COUNTER"
#define FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS  100000

FabricPortsOrch::FabricPortsOrch(DBConnector *appl_db, vector<table_name_with_pri_t> &tableNames,
                                 bool fabricPortStatEnabled, bool fabricQueueStatEnabled) :
        Orch(appl_db, tableNames),
        port_stat_manager(FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                          FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        queue_stat_manager(FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                           FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true)
{
}

bool FabricPortsOrch::getPort(sai_object_id_t id, Port &port)
{
    for (const auto &p : m_fabricLanePortMap)
    {
        if (p.second == id)
        {
            return true;
        }
    }
    return false;
}

bool FabricPortsOrch::allPortsReady()
{
    return true;
}

void FabricPortsOrch::doTask()
{
}

void FabricPortsOrch::doTask(Consumer &consumer)
{
}

void FabricPortsOrch::doTask(swss::SelectableTimer &timer)
{
}
