#ifndef __SWITCHCONF__
#define __SWITCHCONF__

#include "dbconnector.h"
#include "orchbase.h"

#include <map>
#include <string>

namespace swss {


class SwitchConf : public OrchBase
{
public:
    SwitchConf(DBConnector *cfgDb, DBConnector *appDb, string tableName);
    virtual ~SwitchConf();
    void updateHostFloodControl(string brif);
    void syncCfgDB();
private:
    bool m_unicast_miss_flood = true;
    bool m_multicast_miss_flood = true;
    bool m_broadcast_flood = true;

    shared_ptr<ProducerStateTable> m_appSwitchTableProducer = nullptr;
	Table m_cfgSwitchTable;

    void doTask(Consumer &consumer);
    void getHostFloodSetting(bool &flood, string &action);
};

}

#endif
