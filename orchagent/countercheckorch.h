#ifndef COUNTERCHECK_ORCH_H
#define COUNTERCHECK_ORCH_H

#include "orchagent/orch.h"
#include "orchagent/port.h"
#include "orchagent/timer.h"
#include <array>

#define PFC_WD_TC_MAX 8

extern "C" {
#include "sai.h"
}

typedef std::vector<uint64_t> QueueMcCounters;
typedef std::array<uint64_t, PFC_WD_TC_MAX> PfcFrameCounters;

class CounterCheckOrch: public Orch
{
public:
    static CounterCheckOrch& getInstance(swss::DBConnector *db = nullptr);
    virtual void doTask(swss::SelectableTimer &timer);
    virtual void doTask(Consumer &consumer) {}
    void addPort(const swss::Port& port);
    void removePort(const swss::Port& port);

private:
    CounterCheckOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~CounterCheckOrch(void);
    QueueMcCounters getQueueMcCounters(const swss::Port& port);
    PfcFrameCounters getPfcFrameCounters(sai_object_id_t portId);
    void mcCounterCheck();
    void pfcFrameCounterCheck();

    std::map<sai_object_id_t, QueueMcCounters> m_mcCountersMap;
    std::map<sai_object_id_t, PfcFrameCounters> m_pfcFrameCountersMap;

    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    std::shared_ptr<swss::Table> m_countersTable = nullptr;
};

#endif
