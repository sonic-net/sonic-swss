#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <system_error>
#include <iostream>
#include <set>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "cfgagent/switchcfgagent.h"
#include "exec.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define CFG_SWITCH_FLOOD_CONTROL_KEY_NAME "FLOOD_CONTROL"

SwitchCfgAgent::SwitchCfgAgent(DBConnector *cfgDb, DBConnector *appDb, string tableName) :
        CfgOrch(cfgDb, tableName),
        m_cfgSwitchTableConsumer(cfgDb, tableName),
        m_switchTableProducer(appDb, APP_SWITCH_TABLE_NAME)
{

}

void SwitchCfgAgent::syncCfgDB()
{
    CfgOrch::syncCfgDB(CFG_SWITCH_TABLE_NAME, m_cfgSwitchTableConsumer);
}

// ls /sys/class/net/Bridge/brif/ | xargs -n 1 -I % sh -c 'echo %; echo 0 > /sys/class/net/Bridge/brif/%/unicast_flood'
void SwitchCfgAgent::updateHostFloodControl(string brif)
{
    string brif_path = "/sys/class/net/";
    string cmd;
    string unicast_op, multicast_op, broadcast_op;
    struct stat sb;

    brif_path += DOT1Q_BRIDGE_NAME;
    brif_path += "/brif/";

    if (stat(brif_path.c_str(), &sb) || !S_ISDIR(sb.st_mode))
    {
       SWSS_LOG_INFO("updateHostFloodControl: %s doens't exist", brif_path.c_str());
       return;
    }

    unicast_op = (m_unicast_miss_flood ? " 1 > " : " 0 > ");
    multicast_op = (m_multicast_miss_flood ? " 1 > " : " 0 > ");
    broadcast_op = (m_broadcast_flood ? " 1 > " : " 0 > ");

     // TODO: optimize the repeated CLI processing
    if (brif.empty())
    {
        string cmd_prefix;
        //Apply to all bridge ports in 1q bridge
        cmd_prefix = "ls " + brif_path;
        cmd_prefix += " | xargs -n 1 -I % sh -c 'echo";

        cmd = cmd_prefix + unicast_op;
        cmd += brif_path + "%/unicast_flood'";
        swss::exec(cmd.c_str());

        cmd = cmd_prefix + multicast_op;
        cmd += brif_path + "%/multicast_flood'";
        swss::exec(cmd.c_str());

        cmd = cmd_prefix + broadcast_op;
        cmd += brif_path + "%/broadcast_flood'";
        swss::exec(cmd.c_str());
    }
    else
    {
        cmd = "echo" + unicast_op;
        cmd += brif_path + brif + "/unicast_flood";
        exec(cmd.c_str());

        cmd = "echo" + multicast_op;
        cmd += brif_path + brif + "/multicast_flood";
        swss::exec(cmd.c_str());

        cmd = "echo" + broadcast_op;
        cmd += brif_path + brif + "/broadcast_flood";
        swss::exec(cmd.c_str());
    }
}

void SwitchCfgAgent::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);

        SWSS_LOG_DEBUG("Switch doTask: %s", (dumpTuple(consumer, t)).c_str());

        if (key == CFG_SWITCH_FLOOD_CONTROL_KEY_NAME)
        {
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "unicast_miss_flood") {
                    m_unicast_miss_flood = (fvValue(i)=="false" ? false:true);
                }
                else  if (fvField(i) == "multicast_miss_flood") {
                    m_multicast_miss_flood = (fvValue(i)=="false" ? false:true);
                }
                else if (fvField(i) == "broadcast_flood") {
                    m_broadcast_flood = (fvValue(i)=="false" ? false:true);
                }
            }
            string all_brif;
            updateHostFloodControl(all_brif);
        }

        m_switchTableProducer.set(key, kfvFieldsValues(t));
        SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());

        it = consumer.m_toSync.erase(it);
            continue;
    }
}