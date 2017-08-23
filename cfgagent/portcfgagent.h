#ifndef __PORTCFGAGENT__
#define __PORTCFGAGENT__

#include "dbconnector.h"
#include "cfgorch.h"

#include <map>
#include <string>

namespace swss {


class PortCfgAgent : public CfgOrch
{
public:
    PortCfgAgent(DBConnector *cfgDb, DBConnector *appDb, string tableName);

private:
	ProducerStateTable m_portTableProducer;

    void doTask(Consumer &consumer);
    bool setHostPortAdminState(string &alias, string &admin_status);
    bool setHostPortMtu(string &alias, uint32_t mtu);
    bool setHostPortPvid(string &alias, uint32_t pvid);
};

}

#endif
