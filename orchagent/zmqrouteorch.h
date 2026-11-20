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
    void dumpPendingTasks(std::vector<std::string> &ts) override;

    /* Matches drain()'s run condition: while a yielded route drain batch is
     * in flight, m_toSync can be empty because the batch owns its entries. */
    bool hasPendingTasks() const override;

    /* Re-arm this consumer's selectable so select() returns at once. The
     * yielding route drain calls it when it stops with work left; other ready
     * selectables of equal or higher priority are served first. */
    void notifyPending()
    {
        static_cast<swss::ZmqRouteConsumerStateTable *>(getSelectable())->notifyPending();
    }

    /* True if the mqPollThread has staged tuples not yet merged into
     * m_toSync. The notification for them may have been consumed by an
     * execute() that could not merge while a drain batch was in flight. */
    bool ingressPending()
    {
        std::lock_guard<std::mutex> lk(m_ingressMutex);
        return !m_ingress.empty();
    }

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

    /* True while a drain batch for tableName is between yields.
     * ZmqRouteConsumer::execute() does not merge m_ingress into m_toSync
     * then, so m_toSync changes only through the drain for the whole batch. */
    virtual bool drainInFlight(const std::string &tableName) const { return false; }

private:
    void addConsumer(swss::DBConnector *db, std::string tableName, int pri, swss::ZmqRouteServer *zmqServer);
};
