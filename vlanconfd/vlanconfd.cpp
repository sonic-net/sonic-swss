#include <unistd.h>
#include <vector>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "macaddress.h"
#include "producerstatetable.h"
#include "switchconfvlan.h"
#include "vlanconf.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

MacAddress gMacAddress;
SwitchConfVlan *gSwtichConfVlan;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("vlanconfd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting vlanconfd ---");

    string mac_str, cfg_mac_str;
    swss::exec("ip link show eth0 | grep ether | awk '{print $2}'", mac_str);
    gMacAddress = mac_str;

    try
    {
        vector<string> cfg_vlan_tables = {
            CFG_VLAN_TABLE_NAME,
            CFG_VLAN_MEMBER_TABLE_NAME,
        };

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        gSwtichConfVlan = new SwitchConfVlan(&cfgDb, &appDb, CFG_SWITCH_TABLE_NAME);
        VlanConf vlanconf(&cfgDb, &appDb, &stateDb, cfg_vlan_tables);

        std::vector<CfgOrch *> cfgOrchList = {&vlanconf, gSwtichConfVlan};

        swss::Select s;
        for (CfgOrch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int fd, ret;

            ret = s.select(&sel, &fd, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                ((CfgOrch *)&vlanconf)->doTask();
                continue;
            }

            for (CfgOrch *o : cfgOrchList)
            {
                TableConsumable *c = (TableConsumable *)sel;
                if (o->hasSelectable(c))
                {
                    o->execute(c->getTableName());
                }
            }
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return 0;
}
