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

class ZmqRouteConsumer : public ConsumerBase {
public:
    ZmqRouteConsumer(swss::ZmqRouteConsumerStateTable *select, Orch *orch, const std::string &name);

    swss::TableBase *getConsumerTable() const override
    {
        // ZmqRouteConsumerStateTable is a subclass of TableBase
        return static_cast<swss::ZmqRouteConsumerStateTable *>(getSelectable());
    }

    void execute() override;
    void drain() override;

/* protected rather than private so the unit tests (which compile with
 * protected mapped to public) can assert staging invariants such as
 * m_ingress.empty() after a drain. */
protected:
    // Staging buffer for tuples delivered by the ZmqRouteServer mqPollThread
    // ingress callback. The callback writes here (rather than merging into
    // m_toSync directly); execute() drains it into m_toSync.
    //
    // Threading invariant: m_ingressMutex guards m_ingress only. m_toSync is
    // owned exclusively by the orch main thread (execute() -> drain() ->
    // doTask()), and no lock is held across doTask(). That is what keeps
    // re-entrant addToSync() calls made from inside doTask() -- e.g. the route
    // resync path in RouteOrch::doTask() -- safe rather than a self-deadlock,
    // and it lets the base ConsumerBase paths stay lock-free.
    std::mutex m_ingressMutex;
    std::unordered_map<std::string, swss::KeyOpFieldsValuesTuple> m_ingress;
};

class ZmqRouteOrch : public Orch
{
public:
    ZmqRouteOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, swss::ZmqRouteServer *zmqServer);
    ZmqRouteOrch(swss::DBConnector *db, const std::vector<table_name_with_pri_t> &tableNames_with_pri, swss::ZmqRouteServer *zmqServer);

    virtual void doTask(ConsumerBase &consumer) { };
    void doTask(Consumer &consumer) override;

private:
    void addConsumer(swss::DBConnector *db, std::string tableName, int pri, swss::ZmqRouteServer *zmqServer);
};
