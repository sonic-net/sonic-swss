#pragma once

#include <set>
#include <string>

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"
#include <sys/types.h>

namespace swss {

class TeamMgr : public Orch
{
public:
    TeamMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *staDb,
            const std::vector<TableConnector> &tables);

    using Orch::doTask;
    void cleanTeamProcesses();

private:
    Table m_cfgMetadataTable;   // To retrieve MAC address
    Table m_cfgPortTable;
    Table m_cfgLagTable;
    Table m_cfgLagMemberTable;
    Table m_statePortTable;
    Table m_stateLagTable;
    Table m_stateMACsecIngressSATable;

    ProducerStateTable m_appPortTable;
    ProducerStateTable m_appLagTable;

    std::set<std::string> m_lagList;

    MacAddress m_mac;

    void doTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);
    void doPortUpdateTask(Consumer &consumer);

    task_process_status addLag(const std::string &alias, int min_links, bool fall_back);
    bool removeLag(const std::string &alias);
    task_process_status addLagMember(const std::string &lag, const std::string &member);
    bool removeLagMember(const std::string &lag, const std::string &member);

    bool setLagAdminStatus(const std::string &alias, const std::string &admin_status);
    bool setLagMtu(const std::string &alias, const std::string &mtu);
    bool setLagLearnMode(const std::string &alias, const std::string &learn_mode);
    bool setLagTpid(const std::string &alias, const std::string &tpid);

    bool isPortEnslaved(const std::string &);
    bool findPortMaster(std::string &, const std::string &);
    bool checkPortIffUp(const std::string &);
    bool isPortStateOk(const std::string&);
    bool isLagStateOk(const std::string&);
    bool isMACsecAttached(const std::string &);
    bool isMACsecIngressSAOk(const std::string &);
    uint16_t generateLacpKey(const std::string&);
};

}
