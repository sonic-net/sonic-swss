#ifndef MC_WATCHDOG_H
#define MC_WATCHDOG_H

#include "orch.h"
#include "port.h"
#include "timer.h"

extern "C" {
#include "sai.h"
}

typedef vector<uint64_t> QueueMcCounters;

class McWdOrch: public Orch
{
public:
    static McWdOrch& getInstance(DBConnector *db = nullptr);
    virtual void doTask(SelectableTimer &timer);
    virtual void doTask(Consumer &consumer) {}
    void addPort(const Port& port);
    void removePort(const Port& port);

private:
    McWdOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~McWdOrch(void);
    QueueMcCounters getQueueMcCounters(const Port& port);
    map<sai_object_id_t, QueueMcCounters> m_CountersMap;

    shared_ptr<DBConnector> m_countersDb = nullptr;
    shared_ptr<Table> m_countersTable = nullptr;
};

#endif
