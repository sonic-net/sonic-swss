#ifndef __INTFCFGORCH__
#define __INTFCFGORCH__

#include "dbconnector.h"
#include "cfgorch.h"

#include <map>
#include <string>

namespace swss {


class IntfCfgAgent : public CfgOrch
{
public:
    IntfCfgAgent(DBConnector *cfgDb, DBConnector *appDb, string tableName);

private:
	ProducerStateTable m_intfTableProducer;

    bool setIntfIp(string &alias, string &opCmd, string &ipPrefixStr);
    void doTask(Consumer &consumer);
};

}

#endif
