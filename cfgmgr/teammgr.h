#pragma once

#include <set>
#include <string>

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"

namespace swss {

class TeamMgr : public Orch
{
public:
    TeamMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *staDb,
            const vector<TableConnector> &tables);

    using Orch::doTask;
private:
    Table m_cfgMetadataTable;   // To retrieve MAC address
    Table m_cfgPortTable;
    Table m_cfgLagTable;
    Table m_cfgLagMemberTable;
    Table m_statePortTable;
    Table m_stateLagTable;

    ProducerStateTable m_appPortTable;
    ProducerStateTable m_appLagTable;

    set<string> m_portList;
    set<string> m_lagList;

    MacAddress m_mac;

    void doTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);
    void doPortUpdateTask(Consumer &consumer);

    bool addLag(const string &alias, int min_links, bool fall_back);
    bool removeLag(const string &alias);
    bool addLagMember(const string &lag, const string &member);
    bool removeLagMember(const string &lag, const string &member);

    bool setLagAdminStatus(const string &alias, const string &admin_status);
    bool setLagMtu(const string &alias, const string &mtu);

    bool isPortStateOk(const string&);
    bool isLagStateOk(const string&);
};

}
