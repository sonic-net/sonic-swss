#ifndef __SWITCHCONFVLAN__
#define __SWITCHCONFVLAN__

#include "dbconnector.h"
#include "cfgorch.h"

#include <map>
#include <string>

namespace swss {


class SwitchConfVlan : public CfgOrch
{
public:
    SwitchConfVlan(DBConnector *cfgDb, DBConnector *appDb, string tableName);
    virtual ~SwitchConfVlan();
    void updateHostFloodControl(string brif);
    void syncCfgDB();
private:
    bool m_unicast_miss_flood = true;
    bool m_multicast_miss_flood = true;
    bool m_broadcast_flood = true;

	ProducerStateTable m_appSwitchTableProducer;
	Table m_cfgSwitchTableConsumer;

    void doTask(Consumer &consumer);
    void getHostFloodSetting(bool &flood, string &action);
};

}

#endif
