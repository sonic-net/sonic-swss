#include "zmqorch.h"

using namespace swss;
using namespace std;

extern int gBatchSize;
extern bool gSwssRecord;

void ZmqConsumer::execute()
{
    SWSS_LOG_ENTER();

    size_t update_size = 0;
    auto table = static_cast<swss::ZmqConsumerStateTable*>(getSelectable());
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
    if (!m_toSync.empty() || !m_queue.empty())
        (static_cast<ZmqOrch*>(m_orch))->doTask(*this);
}

ZmqOrch::ZmqOrch(DBConnector *db, const vector<string> &tableNames, ZmqServer *zmqServer, bool orderedQueue, bool dbPersistence)
: Orch()
{
    for (auto it : tableNames)
    {
        addConsumer(db, it, default_orch_pri, zmqServer, orderedQueue, dbPersistence);
    }
}

void ZmqOrch::addConsumer(DBConnector *db, string tableName, int pri, ZmqServer *zmqServer, bool orderedQueue, bool dbPersistence)
{
    if (db->getDbId() == APPL_DB)
    {
        if (zmqServer != nullptr)
        {
            SWSS_LOG_DEBUG("ZmqConsumer initialize for: %s", tableName.c_str());
            addExecutor(new ZmqConsumer(new ZmqConsumerStateTable(db, tableName, *zmqServer, gBatchSize, pri, dbPersistence), this, tableName, orderedQueue));
        }
        else
        {
            SWSS_LOG_DEBUG("Consumer initialize for: %s", tableName.c_str());
            addExecutor(new Consumer(new ConsumerStateTable(db, tableName, gBatchSize, pri), this, tableName));
        }
    }
    else
    {
        SWSS_LOG_WARN("ZmqOrch does not support create consumer for db: %d, table: %s", db->getDbId(), tableName.c_str());
    }
}

void ZmqOrch::doTask(Consumer &consumer)
{
    // When ZMQ disabled, forward data from Consumer
    doTask((ConsumerBase &)consumer);
}
