#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>

namespace swss {

class IntfMgr : public Orch
{
public:
    IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appIntfTableProducer;
    Table m_cfgIntfTable, m_cfgVlanIntfTable;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateVrfTable, m_stateIntfTable;

    void setIntfIp(const std::string &alias, const std::string &opCmd, const IpPrefix &ipPrefix);
    void setIntfVrf(const std::string &alias, const std::string vrfName);
    bool doIntfGeneralTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op);
    bool doIntfAddrTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op);
    void doTask(Consumer &consumer);
    bool isIntfStateOk(const std::string &alias);
};

}

#endif
