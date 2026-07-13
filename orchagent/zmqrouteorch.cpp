#include "zmqrouteorch.h"

using namespace swss;
using namespace std;

extern int gBatchSize;
extern size_t gMaxBulkSize;

ZmqRouteConsumer::ZmqRouteConsumer(ZmqRouteConsumerStateTable *select, Orch *orch, const std::string &name)
    : ConsumerBase(select, orch, name)
{
    // mqPollThread delivers bursts of tuples through this callback. Stage them
    // in the plain m_ingress map under m_toSyncMutex rather than merging into
    // m_toSync here; the merge into m_toSync happens on the orch main thread in
    // execute(). The eventfd is fired once the staged batch reaches
    // gMaxBulkSize (so the main loop has a real batch to drain); otherwise
    // mqPollThread fires it once per burst after the burst quiesces.
    select->setIngressCallback(
        [this, select](const std::vector<std::shared_ptr<KeyOpFieldsValuesTuple>> &kcos) {
            std::lock_guard<std::mutex> lk(m_toSyncMutex);
            for (const auto &kco : kcos)
            {
                // Plain last-writer-wins staging by key. The SyncMap merge into
                // m_toSync is applied later by execute()'s addToSync().
                m_ingress[kfvKey(*kco)] = *kco;
            }
            if (m_ingress.size() >= gMaxBulkSize)
            {
                select->notifyPending();
            }
        });
}

void ZmqRouteConsumer::execute()
{
    SWSS_LOG_ENTER();

    {
        // Drain the staged tuples into m_toSync under the lock, mirroring
        // ZmqConsumer::execute()'s pops() + addToSync(entries). The lock is
        // held only while moving tuples out of m_ingress.
        std::lock_guard<std::mutex> lk(m_toSyncMutex);
        std::deque<KeyOpFieldsValuesTuple> entries;
        for (auto &kv : m_ingress)
        {
            entries.push_back(std::move(kv.second));
        }
        m_ingress.clear();
        addToSync(entries);
    }

    // m_toSync is mutated only by this (main) thread, so drain() — which reads
    // m_toSync and hands it to doTask — does not need to hold m_toSyncMutex.
    drain();
}

void ZmqRouteConsumer::drain()
{
    if (!m_toSync.empty())
        (static_cast<ZmqRouteOrch*>(m_orch))->doTask(*this);
}


ZmqRouteOrch::ZmqRouteOrch(DBConnector *db, const vector<string> &tableNames, ZmqRouteServer *zmqServer)
: Orch()
{
    for (auto it : tableNames)
    {
        addConsumer(db, it, default_orch_pri, zmqServer);
    }
}


ZmqRouteOrch::ZmqRouteOrch(DBConnector *db, const vector<table_name_with_pri_t> &tableNames_with_pri, ZmqRouteServer *zmqServer)
{
    for (const auto& it : tableNames_with_pri)
    {
        addConsumer(db, it.first, it.second, zmqServer);
    }
}

void ZmqRouteOrch::addConsumer(DBConnector *db, string tableName, int pri, ZmqRouteServer *zmqServer)
{
    if (db->getDbId() == APPL_DB || db->getDbId() == DPU_APPL_DB)
    {
        if (zmqServer != nullptr)
        {
            SWSS_LOG_DEBUG("ZmqRouteConsumer initialize for: %s", tableName.c_str());
            addExecutor(
                new ZmqRouteConsumer(
                  new ZmqRouteConsumerStateTable(
                    db, tableName, *zmqServer, pri, /* dbPersistence= */false),
                this, tableName));
        }
        else
        {
            SWSS_LOG_DEBUG("Consumer initialize for: %s", tableName.c_str());
            addExecutor(new Consumer(new ConsumerStateTable(db, tableName, gBatchSize, pri), this, tableName));
        }
    }
    else
    {
        SWSS_LOG_WARN("ZmqRouteOrch does not support create consumer for db: %d, table: %s", db->getDbId(), tableName.c_str());
    }
}

void ZmqRouteOrch::doTask(Consumer &consumer)
{
    // When ZMQ disabled, forward data from Consumer
    doTask((ConsumerBase &)consumer);
}
