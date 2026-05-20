#include "zmqrouteorch.h"

using namespace swss;
using namespace std;

extern int gBatchSize;
extern size_t gMaxBulkSize;

ZmqRouteConsumer::ZmqRouteConsumer(ZmqRouteConsumerStateTable *select, Orch *orch, const std::string &name)
    : ConsumerBase(select, orch, name)
{
    // mqPollThread runs the merge inline: kcos go straight into m_toSync
    // under m_toSyncMutex. The eventfd is fired only when m_toSync grows past
    // gMaxBulkSize (so the orch main loop has a real batch to drain);
    // otherwise mqPollThread fires it once per burst after the burst quiesces.
    select->setIngressCallback(
        [this, select](const std::vector<std::shared_ptr<KeyOpFieldsValuesTuple>> &kcos) {
            std::lock_guard<std::mutex> lk(m_toSyncMutex);
            for (const auto &kco : kcos)
            {
                // Qualified call to bypass our own virtual override (which
                // would re-acquire m_toSyncMutex per entry).
                ConsumerBase::addToSync(*kco, /*onRetry=*/false);
            }
            if (m_toSync.size() >= gMaxBulkSize)
            {
                select->notifyPending();
            }
        });
}

void ZmqRouteConsumer::execute()
{
    SWSS_LOG_ENTER();

    // Tuples were already merged into m_toSync by the ingress callback running
    // on mqPollThread. The main loop's job is just to drain.
    drain();
}

void ZmqRouteConsumer::drain()
{
    std::lock_guard<std::mutex> lk(m_toSyncMutex);
    if (!m_toSync.empty())
        (static_cast<ZmqRouteOrch*>(m_orch))->doTask(*this);
}

void ZmqRouteConsumer::addToSync(const KeyOpFieldsValuesTuple &entry, bool onRetry)
{
    std::lock_guard<std::mutex> lk(m_toSyncMutex);
    ConsumerBase::addToSync(entry, onRetry);
}

size_t ZmqRouteConsumer::addToSync(const std::deque<KeyOpFieldsValuesTuple> &entries, bool onRetry)
{
    std::lock_guard<std::mutex> lk(m_toSyncMutex);
    return ConsumerBase::addToSync(entries, onRetry);
}

void ZmqRouteConsumer::dumpPendingTasks(std::vector<std::string> &ts)
{
    std::lock_guard<std::mutex> lk(m_toSyncMutex);
    ConsumerBase::dumpPendingTasks(ts);
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
