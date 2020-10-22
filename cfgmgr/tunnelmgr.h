#pragma once

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

namespace swss {

class TunnelMgr : public Orch
{
public:
    TunnelMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    void doTask(Consumer &consumer);

    bool doIpInIpTunnelTask(const KeyOpFieldsValuesTuple & t);

    ProducerStateTable m_appIpInIpTunnelTable;
};

}
