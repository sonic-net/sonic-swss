#ifndef __SWITCHCFGAGENT__
#define __SWITCHCFGAGENT__

#include "dbconnector.h"
#include "cfgorch.h"

#include <map>
#include <string>

namespace swss {


class SwitchCfgAgent : public CfgOrch
{
public:
    SwitchCfgAgent(DBConnector *cfgDb, DBConnector *appDb, string tableName);

    void updateHostFloodControl(string brif);
    void syncCfgDB();
private:
    bool m_unicast_miss_flood = true;
    bool m_multicast_miss_flood = true;
    bool m_broadcast_flood = true;

	ProducerStateTable m_switchTableProducer;
	Table m_cfgSwitchTableConsumer;

    void doTask(Consumer &consumer);
};

}

#endif
