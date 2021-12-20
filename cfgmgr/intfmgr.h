#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>
#include <set>

struct SubIntfInfo
{
    std::string vlanId;
    std::string mtu;
    std::string adminStatus;
    std::string currAdminStatus;
};

typedef std::map<std::string, SubIntfInfo>             SubIntfMap;

namespace swss {

class IntfMgr : public Orch
{
public:
    IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appIntfTableProducer;
    Table m_cfgIntfTable, m_cfgVlanIntfTable, m_cfgLagIntfTable, m_cfgLoopbackIntfTable;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateVrfTable, m_stateIntfTable, m_appLagTable;
    Table m_neighTable;

    SubIntfMap m_subIntfList;
    std::set<std::string> m_loopbackIntfList;
    std::set<std::string> m_pendingReplayIntfList;
    std::set<std::string> m_ipv6LinkLocalModeList;

    void setIntfIp(const std::string &alias, const std::string &opCmd, const IpPrefix &ipPrefix);
    void setIntfVrf(const std::string &alias, const std::string &vrfName);
    void setIntfMac(const std::string &alias, const std::string &macAddr);
    bool setIntfMpls(const std::string &alias, const std::string &mpls);

    bool doIntfGeneralTask(const std::vector<std::string>& keys, std::vector<FieldValueTuple> data, const std::string& op);
    bool doIntfAddrTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op);
    void doTask(Consumer &consumer);
    void doPortTableTask(const std::string& key, std::vector<FieldValueTuple> data, std::string op);

    bool isIntfStateOk(const std::string &alias);
    bool isIntfCreated(const std::string &alias);
    bool isIntfChangeVrf(const std::string &alias, const std::string &vrfName);
    int getIntfIpCount(const std::string &alias);
    void buildIntfReplayList(void);
    void setWarmReplayDoneState();

    void addLoopbackIntf(const std::string &alias);
    void delLoopbackIntf(const std::string &alias);
    void flushLoopbackIntfs(void);

    std::string getIntfAdminStatus(const std::string &alias);
    std::string getIntfMtu(const std::string &alias);
    void addHostSubIntf(const std::string&intf, const std::string &subIntf, const std::string &vlan);
    std::string setHostSubIntfMtu(const std::string &alias, const std::string &mtu, const std::string &parent_mtu);
    std::string setHostSubIntfAdminStatus(const std::string &alias, const std::string &admin_status, const std::string &parent_admin_status);
    void removeHostSubIntf(const std::string &subIntf);
    void setSubIntfStateOk(const std::string &alias);
    void removeSubIntfState(const std::string &alias);
    void delIpv6LinkLocalNeigh(const std::string &alias);

    bool setIntfProxyArp(const std::string &alias, const std::string &proxy_arp);
    bool setIntfGratArp(const std::string &alias, const std::string &grat_arp);

    void updateSubIntfAdminStatus(const std::string &alias, const std::string &admin);
    void updateSubIntfMtu(const std::string &alias, const std::string &mtu);

    bool m_replayDone {false};
};

}

#endif
