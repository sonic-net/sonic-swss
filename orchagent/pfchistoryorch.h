#ifndef PFCHISTORY_ORCH_H
#define PFCHISTORY_ORCH_H

#include "orch.h"

#define PFC_STAT_HISTORY_FLEX_COUNTER_GROUP "PFC_STAT_HISTORY"

extern "C" {
#include "sai.h"
}

class PfcHistoryOrch: public Orch
{
public:
    static PfcHistoryOrch& getInstance(swss::DBConnector *db = nullptr);
    virtual void doTask(Consumer& consumer);

private:
    PfcHistoryOrch(
        swss::DBConnector *db,
        std::vector<std::string> &tableNames,
        const std::vector<sai_port_stat_t> &portStatIds
    );
    virtual ~PfcHistoryOrch(void);
    static std::string counterIdsToStr(const std::vector<sai_port_stat_t> ids);
    void removeAllPfcStatHistoryCounters();

    const std::vector<sai_port_stat_t> c_portStatIds;
    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
};

#endif
