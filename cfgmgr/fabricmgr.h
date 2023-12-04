#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {


class FabricMgr : public Orch
{
public:
    FabricMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames);

    using Orch::doTask;
private:
    Table m_cfgFabricMonitorTable;
    Table m_cfgFabricPortTable;
    Table m_appFabricMonitorTable;
    Table m_appFabricPortTable;

    void doTask(Consumer &consumer);
    bool writeConfigToAppDb(const std::string &alias, const std::string &field, const std::string &value);
};

}
