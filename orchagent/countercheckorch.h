#ifndef COUNTERCHECK_ORCH_H
#define COUNTERCHECK_ORCH_H

#include "orch.h"
#include "port.h"
#include "timer.h"

extern "C" {
#include "sai.h"
}

typedef vector<uint64_t> QueueMcCounters;

class CounterCheckOrch: public Orch
{
public:
    static CounterCheckOrch& getInstance(DBConnector *db = nullptr);
    virtual void doTask(SelectableTimer &timer);
    virtual void doTask(Consumer &consumer) {}
    void addPort(const Port& port);
    void removePort(const Port& port);

private:
    CounterCheckOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~CounterCheckOrch(void);
    QueueMcCounters getQueueMcCounters(const Port& port);
    map<sai_object_id_t, QueueMcCounters> m_CountersMap;

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
};

#endif
