#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace swss {

/* Port default admin status is down */
#define DEFAULT_ADMIN_STATUS_STR    "down"
#define DEFAULT_MTU_STR             "9100"

class PortMgr : public Orch
{
public:
    PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<TableConnector> &tables);

    using Orch::doTask;
private:
    Table m_cfgPortTable;
    Table m_cfgSendToIngressPortTable;
    Table m_statePortTable;
    Table m_stateMonitorLinkGroupMemberTable;
    ProducerStateTable m_appPortTable;
    ProducerStateTable m_appSendToIngressPortTable;
    std::set<std::string> m_portList;

    void doTask(Consumer &consumer);
    void doSendToIngressPortTask(Consumer &consumer);
    void doMonitorLinkGroupMemberTask(Consumer &consumer);
    bool writeConfigToAppDb(const std::string &alias, const std::string &field, const std::string &value);
    bool writeConfigToAppDb(const std::string &alias, std::vector<FieldValueTuple> &field_values);
    bool setPortMtu(const std::string &alias, const std::string &mtu);
    bool setPortAdminStatus(const std::string &alias, const bool up);
    bool isPortStateOk(const std::string &alias);
    void applyEffectiveAdminStatus(const std::string &alias);
};

}
