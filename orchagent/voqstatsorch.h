#ifndef SWSS_VOQSTATSORCH_H
#define SWSS_VOQSTATSORCH_H

#include "orch.h"
#include "dbconnector.h"
#include "table.h"

class VoqStatsOrch : public Orch
{
public:

    VoqStatsOrch(
        swss::DBConnector *countersDb,
        swss::DBConnector *chassisCountersDb,
        const std::vector<std::string> &tableNames);

private:

    swss::Table m_voqCntrsTable;
    virtual void doTask(Consumer &consumer);
    void updateChassisCountersDb(const std::string &, const std::vector<swss::FieldValueTuple> &);
};

#endif
