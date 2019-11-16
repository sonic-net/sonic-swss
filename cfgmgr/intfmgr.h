#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>
#include <set>

namespace swss {

class IntfMgr : public Orch
{
public:
    IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appIntfTableProducer;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateVrfTable, m_stateIntfTable;

    std::set<std::string> m_subIntfList;

    void setIntfIp(const std::string &alias, const std::string &opCmd, const IpPrefix &ipPrefix);
    void setIntfVrf(const std::string &alias, const std::string &vrfName);
    void setIntfMac(const std::string &alias, const std::string &macAddr);
    
    bool doIntfGeneralTask(const std::vector<std::string>& keys, std::vector<FieldValueTuple> data, const std::string& op);
    bool doIntfAddrTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op);
    void doTask(Consumer &consumer);
    
    bool isIntfStateOk(const std::string &alias);
    bool isIntfCreated(const std::string &alias);
    bool isIntfChangeVrf(const std::string &alias, const std::string &vrfName);
    int getIntfIpCount(const std::string &alias);
    
    void addLoopbackIntf(const std::string &alias);
    void delLoopbackIntf(const std::string &alias);

    void addHostSubIntf(const std::string&intf, const std::string &subIntf, const std::string &vlan);
    void setHostSubIntfMtu(const std::string &subIntf, const std::string &mtu);
    void setHostSubIntfAdminStatus(const std::string &subIntf, const std::string &admin_status);
    void removeHostSubIntf(const std::string &subIntf);
    void setSubIntfStateOk(const std::string &alias);
    void removeSubIntfState(const std::string &alias);
};

}

#endif
