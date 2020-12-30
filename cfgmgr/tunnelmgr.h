#pragma once

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

namespace swss {

class TunnelMgr : public Orch
{
public:
    TunnelMgr(DBConnector *cfgDb, DBConnector *appDb, std::string tableName);
    using Orch::doTask;

private:
    void doTask(Consumer &consumer);

    bool doIpInIpTunnelTask(const KeyOpFieldsValuesTuple & t);

    ProducerStateTable m_appIpInIpTunnelTable;
};

}
