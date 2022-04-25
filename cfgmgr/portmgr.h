#pragma once

#include "config_cache.h"
#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {

/* Port default admin status is down */
#define DEFAULT_ADMIN_STATUS_STR    "down"
#define DEFAULT_MTU_STR             "9100"

class PortMgr : public Orch
{
public:
    PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);

    using Orch::doTask;
private:
    Table m_cfgPortTable;
    Table m_cfgLagMemberTable;
    Table m_statePortTable;
    ProducerStateTable m_appPortTable;

    ConfigCache m_configCache;
    std::vector<FieldValueTuple> m_retryFields;

    void doTask(Consumer &consumer);
    bool onPortConfigChanged(const std::string &alias, const std::string &field, const std::string &old_value, const std::string &new_value, void *context);
    bool writeConfigToAppDb(const std::string &alias, const std::string &field, const std::string &value);
    bool setPortMtu(const std::string &alias, const std::string &mtu);
    bool setPortAdminStatus(const std::string &alias, const bool up);
    bool isPortStateOk(const std::string &alias);
};

}
