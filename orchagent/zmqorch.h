#pragma once

#include <vector>
#include <string>
#include <deque>
#include <orch.h>
#include "zmqserver.h"

class ZmqConsumer : public ConsumerBase {
public:
    ZmqConsumer(swss::ZmqConsumerStateTable *select, Orch *orch, const std::string &name, bool orderedQueue = false)
        : ConsumerBase(select, orch, name), m_ordered_queue(orderedQueue)
    {
    }

    swss::TableBase *getConsumerTable() const override
    {
        // ZmqConsumerStateTable is a subclass of TableBase
        return static_cast<swss::ZmqConsumerStateTable *>(getSelectable());
    }

    void execute() override;
    void drain() override;


    // If m_ordered_queue is set, m_queue will be used instead of m_toSync for
    // storing requests.
    bool m_ordered_queue;
    std::deque<swss::KeyOpFieldsValuesTuple> m_queue;
};

class ZmqOrch : public Orch
{
public:
    ZmqOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, swss::ZmqServer *zmqServer, bool orderedQueue = false, bool dbPersistence = true);

    virtual void doTask(ConsumerBase &consumer) { };
    void doTask(Consumer &consumer) override;

private:
    void addConsumer(swss::DBConnector *db, std::string tableName, int pri, swss::ZmqServer *zmqServer, bool orderedQueue = false, bool dbPersistence = true);
};
