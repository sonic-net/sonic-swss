#include <string.h>
#include <errno.h>
#include <system_error>
#include <iostream>
#include <set>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "cfgagent/portcfgagent.h"
#include "exec.h"

using namespace std;
using namespace swss;

#define DEFAULT_PORT_VLAN_ID     1

const string INTFS_PREFIX = "Ethernet";
extern bool gInitDone;

PortCfgAgent::PortCfgAgent(DBConnector *cfgDb, DBConnector *appDb, string tableName) :
        CfgOrch(cfgDb, tableName),
        m_portTableProducer(appDb, APP_PORT_TABLE_NAME)
{

}

bool PortCfgAgent::setHostPortAdminState(string &alias, string &admin_status)
{
    string cmd;

    cmd = "ip link set ";
    cmd += alias + " " + admin_status;
    swss::exec(cmd.c_str());
    return true;
}

bool PortCfgAgent::setHostPortMtu(string &alias, uint32_t mtu)
{
    string cmd;

    cmd = "ip link set ";
    cmd += alias + " mtu " + to_string(mtu);
    swss::exec(cmd.c_str());
    return true;
}


bool PortCfgAgent::setHostPortPvid(string &alias, uint32_t pvid)
{
    // TODO: should be set with bridge vlan

    return true;
}

void PortCfgAgent::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    // Don't start port processing until hostifs for physical ports are ready
    if (!gInitDone)
        return;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        // Do we need to check validity of port name here?
        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (alias.compare(0, INTFS_PREFIX.length(), INTFS_PREFIX))
        {
            SWSS_LOG_NOTICE("Invalid port name %s", alias.c_str());
            return;
        }

        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvVector;
            string admin_status;
            uint32_t mtu = 0;
            uint32_t pvid = DEFAULT_PORT_VLAN_ID;

            // set up host env ....
            for (auto i : kfvFieldsValues(t))
            {
                /* Set port admin status */
                if (fvField(i) == "admin_status") {
                    admin_status = fvValue(i);
                    setHostPortAdminState(alias, admin_status);
                }

                /* Set port mtu */
                if (fvField(i) == "mtu") {
                    mtu = stoul(fvValue(i));
                    setHostPortMtu(alias, mtu);
                }

                if (fvField(i) == "pvid") {
                    pvid = stoul(fvValue(i));
                    setHostPortPvid(alias, pvid);
                    // set APPDB directly for pvid config
                    m_portTableProducer.set(alias, kfvFieldsValues(t));
                }
            }

            SWSS_LOG_DEBUG("port doTask: %s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            // Physical port can not be removed ?
            SWSS_LOG_ERROR("port doTask: %s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_ERROR("port doTask: %s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}