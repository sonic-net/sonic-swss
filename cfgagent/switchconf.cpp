#include <string.h>
#include <sys/stat.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "switchconf.h"
#include "exec.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define CFG_SWITCH_ATTR_NAME "SWITCH_ATTR"

SwitchConf::SwitchConf(DBConnector *cfgDb, DBConnector *appDb, string tableName) :
        OrchBase(cfgDb, tableName),
        m_cfgSwitchTable(cfgDb, tableName, CONFIGDB_TABLE_NAME_SEPARATOR)
{
    /*
     * There could be multiple configDB enforcers listening on SWITCH table.
     * Only one of them will pass the configuration down to appDB, others
     * get the config data for local consumtion.
     */
    if (appDb)
    {
        m_appSwitchTableProducer = make_shared<ProducerStateTable>(appDb, APP_SWITCH_TABLE_NAME);
    }
}

SwitchConf::~SwitchConf()
{
}

void SwitchConf::syncCfgDB()
{
    OrchBase::syncDB(CFG_SWITCH_TABLE_NAME, m_cfgSwitchTable);
}

/*
 * ls /sys/class/net/Bridge/brif/ | xargs -n 1 -I % sh -c 'echo %; echo 0 > /sys/class/net/Bridge/brif/%/unicast_flood'
 */
void SwitchConf::updateHostFloodControl(string brif)
{
    string brif_path = "/sys/class/net/";
    string cmd, res;
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

    if (brif.empty())
    {
        string cmd_prefix;
        /* Apply to all bridge ports in 1q bridge */
        cmd_prefix = "ls " + brif_path;
        cmd_prefix += " | xargs -n 1 -I % sh -c 'echo";

        cmd = cmd_prefix + unicast_op;
        cmd += brif_path + "%/unicast_flood'";
        swss::exec(cmd, res);

        cmd = cmd_prefix + multicast_op;
        cmd += brif_path + "%/multicast_flood'";
        swss::exec(cmd, res);

        cmd = cmd_prefix + broadcast_op;
        cmd += brif_path + "%/broadcast_flood'";
        swss::exec(cmd, res);
    }
    else
    {
        cmd = "echo" + unicast_op;
        cmd += brif_path + brif + "/unicast_flood";
        swss::exec(cmd, res);

        cmd = "echo" + multicast_op;
        cmd += brif_path + brif + "/multicast_flood";
        swss::exec(cmd, res);

        cmd = "echo" + broadcast_op;
        cmd += brif_path + brif + "/broadcast_flood";
        swss::exec(cmd, res);
    }
}

void SwitchConf::getHostFloodSetting(bool &flood, string &action)
{
    if (action == "forward" || action == "trap")
    {
        flood = true;
    }
    else
    {
        flood = false;
    }

}

void SwitchConf::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);

        SWSS_LOG_DEBUG("Switch doTask: %s", (dumpTuple(consumer, t)).c_str());

        if (key == CFG_SWITCH_ATTR_NAME)
        {
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "fdb_unicast_miss_packet_action") {
                    getHostFloodSetting(m_unicast_miss_flood, fvValue(i));
                }
                else  if (fvField(i) == "fdb_multicast_miss_packet_action") {
                    getHostFloodSetting(m_multicast_miss_flood, fvValue(i));
                }
                else if (fvField(i) == "fdb_broadcast_miss_packet_action") {
                    getHostFloodSetting(m_broadcast_flood, fvValue(i));
                }
            }
            string all_brif;
            updateHostFloodControl(all_brif);
            if (m_appSwitchTableProducer !=nullptr)
            {
                m_appSwitchTableProducer->set(key, kfvFieldsValues(t));
            }
        }

        it = consumer.m_toSync.erase(it);
        continue;
    }
}