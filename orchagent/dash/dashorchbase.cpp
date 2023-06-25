#include "dashorchbase.h"

using namespace swss;
using namespace std;

extern int gBatchSize;

void ZmqConsumer::execute()
{
    SWSS_LOG_ENTER();

    size_t update_size = 0;
    auto table = static_cast<swss::ZmqConsumerStateTable *>(getSelectable());
    do
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        table->pops(entries);
        update_size = addToSync(entries);
    } while (update_size != 0);

    drain();
}

void ZmqConsumer::drain()
{
    if (!m_toSync.empty())
        ((DashOrchBase *)m_orch)->doTask((ZmqConsumer&)*this);
}


DashOrchBase::DashOrchBase(DBConnector *db, const vector<string> &tableNames, ZmqServer *zmqServer)
: Orch()
{
    for (auto it : tableNames)
    {
        addConsumer(db, it, default_orch_pri, zmqServer);
    }
}

void DashOrchBase::addConsumer(DBConnector *db, string tableName, int pri, ZmqServer *zmqServer)
{
    if (db->getDbId() == APPL_DB)
    {
        if (zmqServer != nullptr)
        {
            SWSS_LOG_DEBUG("ZmqConsumer initialize for: %s", tableName.c_str());
            addExecutor(new ZmqConsumer(new ZmqConsumerStateTable(db, tableName, *zmqServer, gBatchSize, pri), this, tableName));
        }
        else
        {
            SWSS_LOG_DEBUG("Consumer initialize for: %s", tableName.c_str());
            addExecutor(new Consumer(new ConsumerStateTable(db, tableName, gBatchSize, pri), this, tableName));
        }
    }
    else
    {
        SWSS_LOG_WARN("DashOrchBase does not support create consumer for db: %d, table: %s", db->getDbId(), tableName.c_str());
    }
}