#pragma once

#include <map>
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

    void doTask() override;
    void cleanTeamProcesses();
    bool setLagSysmac(const std::string &alias, std::string &sys_mac);

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

    /* Desired per-member macsec_gate, derived from CONFIG_DB + STATE_DB.
     * Closed (false) while MACsec is attached and the member has no ingress SA.
     * m_macsecGatePushed is the last value teamd accepted. An absent pushed
     * entry counts as open: teamd starts every added port fail-open. */
    std::map<std::string, std::map<std::string, bool>> m_macsecMemberGate;
    std::map<std::string, std::map<std::string, bool>> m_macsecGatePushed;

    MacAddress m_mac;

    void doTask(Consumer &consumer) override;
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);
    void doPortUpdateTask(Consumer &consumer);
    void doMacsecIngressSaTask(Consumer &consumer);
    void doMacsecPortTask(Consumer &consumer);

    /* MACsec member pull: drive teamd's per-member runner.macsec_gate from
     * STATE_DB MACsec SA presence, so a member whose MACsec session is down is
     * taken out of the LACP distributor by teamd itself, on both ends, without
     * touching the link. */
    bool hasMACsecIngressSA(const std::string &port);
    bool setLagMemberMacsecGate(const std::string &lag, const std::string &member, bool gate);
    void applyMacsecMemberGate(const std::string &lag, const std::string &member, bool gate);
    void evaluateMacsecMemberGate(const std::string &port);
    void evaluateMacsecMembersOfLag(const std::string &lag);
    void retryMacsecMemberGates();
    void forgetMacsecMemberGate(const std::string &lag, const std::string &member);
    void forgetMacsecPortGates(const std::string &port);

    task_process_status addLag(const std::string &alias, int min_links, bool fall_back, bool fast_rate);
    bool removeLag(const std::string &alias);
    task_process_status addLagMember(const std::string &lag, const std::string &member);
    bool removeLagMember(const std::string &lag, const std::string &member);

    bool setLagAdminStatus(const std::string &alias, const std::string &admin_status);
    bool setLagMtu(const std::string &alias, const std::string &mtu);
    bool setLagLearnMode(const std::string &alias, const std::string &learn_mode);
    bool setLagTpid(const std::string &alias, const std::string &tpid);
    int update_kernel(const std::string &alias, const std::string &system_mac);

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
