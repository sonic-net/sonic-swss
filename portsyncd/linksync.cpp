#include <string.h>
#include <errno.h>
#include <system_error>
#include <sys/socket.h>
#include <net/if.h>
#include <netlink/route/link.h>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "exec.h"

#include "linkcache.h"
#include "portsyncd/linksync.h"
#include "warm_restart.h"
#include "shellcmd.h"

#include <iostream>
#include <set>
#include <sstream>
#include <iomanip>

using namespace std;
using namespace swss;

#define VLAN_DRV_NAME   "bridge"
#define TEAM_DRV_NAME   "team"

const string INTFS_PREFIX = "Ethernet";
const string LAG_PREFIX = "PortChannel";

extern set<string> g_portSet;
extern bool g_init;
extern string g_switchType;

LinkSync::LinkSync(DBConnector *appl_db, DBConnector *state_db) :
    m_portTableProducer(appl_db, APP_PORT_TABLE_NAME),
    m_portTable(appl_db, APP_PORT_TABLE_NAME),
    m_statePortTable(state_db, STATE_PORT_TABLE_NAME)
{
    std::shared_ptr<struct if_nameindex> if_ni(if_nameindex(), if_freenameindex);
    struct if_nameindex *idx_p;

    if (!WarmStart::isWarmStart())
    {
        /* See the comments for g_portSet in portsyncd.cpp */
        for (auto port_iter = g_portSet.begin(); port_iter != g_portSet.end();)
        {
            string port = *port_iter;
            vector<FieldValueTuple> temp;
            bool portFound = false;
            if (m_portTable.get(port, temp))
            {
                for (auto it : temp)
                {
                    if (fvField(it) == "admin_status")
                    {
                        port_iter = g_portSet.erase(port_iter);
                        portFound = true;
                        break;
                    }
                }
            }
            if (!portFound)
            {
                ++port_iter;
            }
        }

        /* In DPU SONiC netdevs in Kernel are created in the early stage of the syncd service start,
         * when the driver is loading. And exist while the driver remains loaded. 
         *The comparison logic to distinguish "old" interfaces is not needed. */
        if (g_switchType == "dpu")
        {
            return;
        }

        for (idx_p = if_ni.get();
                idx_p != NULL && idx_p->if_index != 0 && idx_p->if_name != NULL;
                idx_p++)
        {
            string key = idx_p->if_name;

            /* Skip all non-frontpanel ports */
            if (key.compare(0, INTFS_PREFIX.length(), INTFS_PREFIX))
            {
                continue;
            }

            m_ifindexOldNameMap[idx_p->if_index] = key;

            ostringstream cmd;
            string res;
            /* Bring down the existing kernel interfaces */
            SWSS_LOG_INFO("Bring down old interface %s(%d)", key.c_str(), idx_p->if_index);
            cmd << "ip link set " << quoted(key) << " down";
            try
            {
                swss::exec(cmd.str(), res);
            }
            catch (...)
            {
                /* Ignore error in this flow ; */
                SWSS_LOG_WARN("Failed to bring down old interface %s(%d)", key.c_str(), idx_p->if_index);
            }
        }
    }
}

void LinkSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    SWSS_LOG_ENTER();

    if ((nlmsg_type != RTM_NEWLINK) && (nlmsg_type != RTM_DELLINK))
    {
        return;
    }

    struct rtnl_link *link = (struct rtnl_link *)obj;
    string key = rtnl_link_get_name(link);

    if (key.compare(0, INTFS_PREFIX.length(), INTFS_PREFIX) &&
        key.compare(0, LAG_PREFIX.length(), LAG_PREFIX))
    {
        return;
    }

    unsigned int flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_RUNNING;

    char addrStr[MAX_ADDR_SIZE+1] = {0};
    nl_addr2str(rtnl_link_get_addr(link), addrStr, MAX_ADDR_SIZE);

    unsigned int ifindex = rtnl_link_get_ifindex(link);
    int master = rtnl_link_get_master(link);
    char *type = rtnl_link_get_type(link);
    unsigned int mtu = rtnl_link_get_mtu(link);

    if (type)
    {
        SWSS_LOG_NOTICE("nlmsg type:%d key:%s admin:%d oper:%d addr:%s ifindex:%d master:%d type:%s",
                       nlmsg_type, key.c_str(), admin, oper, addrStr, ifindex, master, type);
    }
    else
    {
        SWSS_LOG_NOTICE("nlmsg type:%d key:%s admin:%d oper:%d addr:%s ifindex:%d master:%d",
                       nlmsg_type, key.c_str(), admin, oper, addrStr, ifindex, master);
    }

    /* teamd instances are dealt in teamsyncd */
    if (type && !strcmp(type, TEAM_DRV_NAME))
    {
        return;
    }

    /* Ignore DELLINK message if port has master, this is applicable to
     * the case where port was part of VLAN bridge or LAG */
    if (master && nlmsg_type == RTM_DELLINK)
    {
        return;
    }

    /* In the event of swss restart, it is possible to get netlink messages during bridge
     * delete, interface delete etc which are part of cleanup. These netlink messages for
     * the front-panel interface must not be published or it will update the statedb with
     * old interface info and result in subsequent failures. Ingore all netlink messages
     * coming from old interfaces.
     */

    if (m_ifindexOldNameMap.find(ifindex) != m_ifindexOldNameMap.end())
    {
        SWSS_LOG_INFO("nlmsg type:%d Ignoring message for old interface %s(%d)",
                nlmsg_type, key.c_str(), ifindex);
        return;
    }

    /* Insert or update the ifindex to key map */
    m_ifindexNameMap[ifindex] = key;

    if (nlmsg_type == RTM_DELLINK)
    {
        m_statePortTable.del(key);
        SWSS_LOG_NOTICE("Delete %s(ok) from state db", key.c_str());
        return;
    }

    /* front panel interfaces: Check if the port is in the PORT_TABLE
     * non-front panel interfaces such as eth0, lo which are not in the
     * PORT_TABLE are ignored. */
    vector<FieldValueTuple> temp;
    if (m_portTable.get(key, temp))
    {
        g_portSet.erase(key);
        FieldValueTuple tuple("state", "ok");
        FieldValueTuple admin_status("admin_status", (admin ? "up" : "down"));
        FieldValueTuple port_mtu("mtu", to_string(mtu));
        vector<FieldValueTuple> vector;
        vector.push_back(tuple);
        FieldValueTuple op("netdev_oper_status", oper ? "up" : "down");
        vector.push_back(op);
        vector.push_back(admin_status);
        vector.push_back(port_mtu);
        m_statePortTable.set(key, vector);
        SWSS_LOG_NOTICE("Publish %s(ok:%s) to state db", key.c_str(), oper ? "up" : "down");
    }
    else
    {
        SWSS_LOG_NOTICE("Cannot find %s in port table", key.c_str());
    }
}
