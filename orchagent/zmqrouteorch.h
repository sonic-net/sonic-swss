#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <orch.h>
#include "zmqrouteserver.h"
#include "zmqrouteconsumerstatetable.h"

extern int gZmqExecuteTimeQuantaMsecs;

class ZmqRouteConsumer : public ConsumerBase {
public:
    ZmqRouteConsumer(ZmqRouteConsumerStateTable *select, Orch *orch, const std::string &name);

    swss::TableBase *getConsumerTable() const override
    {
        // ZmqRouteConsumerStateTable is a subclass of TableBase
        return static_cast<ZmqRouteConsumerStateTable *>(getSelectable());
    }

    void execute() override;
    void drain() override;

    // Locked overrides: take m_toSyncMutex, then forward to ConsumerBase.
    // These guard against the ZmqRouteServer mqPollThread (calling addToSync
    // via the ingress callback) racing with the orch main thread.
    void addToSync(const swss::KeyOpFieldsValuesTuple &entry, bool onRetry=false) override;
    size_t addToSync(const std::deque<swss::KeyOpFieldsValuesTuple> &entries, bool onRetry=false) override;
    void dumpPendingTasks(std::vector<std::string> &ts) override;

private:
    mutable std::mutex m_toSyncMutex;
};

class ZmqRouteOrch : public Orch
{
public:
    ZmqRouteOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, ZmqRouteServer *zmqServer);
    ZmqRouteOrch(swss::DBConnector *db, const std::vector<table_name_with_pri_t> &tableNames_with_pri, ZmqRouteServer *zmqServer);

    virtual void doTask(ConsumerBase &consumer) { };
    void doTask(Consumer &consumer) override;

private:
    void addConsumer(swss::DBConnector *db, std::string tableName, int pri, ZmqRouteServer *zmqServer);
};
