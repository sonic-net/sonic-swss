#include "zmqorch.h"

using namespace swss;
using namespace std;

extern int gBatchSize;

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
    if (!m_toSync.empty())
        (static_cast<ZmqOrch*>(m_orch))->doTask(*this);
}


ZmqOrch::ZmqOrch(DBConnector *db, const vector<string> &tableNames, ZmqServer *zmqServer)
: Orch()
{
    for (auto it : tableNames)
    {
        addConsumer(db, it, default_orch_pri, zmqServer);
    }
}

ZmqOrch::ZmqOrch(DBConnector *db, const vector<table_name_with_pri_t> &tableNames_with_pri, ZmqServer *zmqServer)
{
    for (const auto& it : tableNames_with_pri)
    {
        addConsumer(db, it.first, it.second, zmqServer);
    }
}

void ZmqOrch::addConsumer(DBConnector *db, string tableName, int pri, ZmqServer *zmqServer)
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
        SWSS_LOG_WARN("ZmqOrch does not support create consumer for db: %d, table: %s", db->getDbId(), tableName.c_str());
    }
}

void ZmqOrch::doTask(Consumer &consumer)
{
    // When ZMQ disabled, forward data from Consumer
    doTask((ConsumerBase &)consumer);
}