#ifndef COUNTERCHECK_ORCH_H
#define COUNTERCHECK_ORCH_H

#include "orch.h"
#include "port.h"
#include "timer.h"
#include <array>

#define PFC_WD_TC_MAX 8

extern "C" {
#include "sai.h"
}

typedef vector<uint64_t> QueueMcCounters;
typedef array<uint64_t, PFC_WD_TC_MAX> PfcFrameCounters;

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
    PfcFrameCounters getPfcFrameCounters(sai_object_id_t portId);
    void mcCounterCheck();
    void pfcFrameCounterCheck();

    map<sai_object_id_t, QueueMcCounters> m_mcCountersMap;
    map<sai_object_id_t, PfcFrameCounters> m_pfcFrameCountersMap;

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
};

#endif
