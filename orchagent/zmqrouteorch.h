#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
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

private:
    // Staging buffer for tuples delivered by the ZmqRouteServer mqPollThread
    // ingress callback. The callback writes here under m_toSyncMutex (rather
    // than merging into m_toSync directly); execute() drains it into m_toSync
    // under the same lock. This keeps m_toSync single-threaded (touched only
    // by the orch main thread), so the base ConsumerBase paths need no locking.
    std::mutex m_toSyncMutex;
    std::unordered_map<std::string, swss::KeyOpFieldsValuesTuple> m_ingress;
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
