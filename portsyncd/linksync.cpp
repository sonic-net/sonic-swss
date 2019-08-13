#include <string.h>
#include <errno.h>
#include <system_error>
#include <sys/socket.h>
#include <linux/if.h>
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

using namespace std;
using namespace swss;

#define VLAN_DRV_NAME   "bridge"
#define TEAM_DRV_NAME   "team"

const string MGMT_PREFIX = "eth";
const string INTFS_PREFIX = "Ethernet";
const string LAG_PREFIX = "PortChannel";

extern set<string> g_portSet;
extern bool g_init;

struct if_nameindex
{
    unsigned int if_index;
    char *if_name;
};
extern "C" { extern struct if_nameindex *if_nameindex (void) __THROW; }

LinkSync::LinkSync(DBConnector *appl_db, DBConnector *state_db) :
    m_portTableProducer(appl_db, APP_PORT_TABLE_NAME),
    m_portTable(appl_db, APP_PORT_TABLE_NAME),
    m_statePortTable(state_db, STATE_PORT_TABLE_NAME),
    m_stateMgmtPortTable(state_db, STATE_MGMT_PORT_TABLE_NAME)
{
    struct if_nameindex *if_ni, *idx_p;
    if_ni = if_nameindex();

    for (idx_p = if_ni;
            idx_p != NULL && idx_p->if_index != 0 && idx_p->if_name != NULL;
            idx_p++)
    {
        string key = idx_p->if_name;

        /* Explicitly store management ports oper status into the state database.
         * This piece of information is used by SNMP. */
        if (!key.compare(0, MGMT_PREFIX.length(), MGMT_PREFIX))
        {
            string cmd, res;
            cmd = "cat /sys/class/net/" + key + "/operstate";
            try
            {
                EXEC_WITH_ERROR_THROW(cmd, res);
            }
            catch (...)
            {
                SWSS_LOG_WARN("Failed to get %s oper status", key.c_str());
                continue;
            }

            /* Remove the trailing newline */
            if (res.length() >= 1 && res.at(res.length() - 1) == '\n')
            {
                res.erase(res.length() - 1);
                /* The value of operstate will be either up or down */
                if (res != "up" && res != "down")
                {
                    SWSS_LOG_WARN("Unknown %s oper status %s",
                            key.c_str(), res.c_str());
                }
                FieldValueTuple fv("oper_status", res);
                vector<FieldValueTuple> fvs;
                fvs.push_back(fv);

                m_stateMgmtPortTable.set(key, fvs);
                SWSS_LOG_INFO("Store %s oper status %s to state DB",
                        key.c_str(), res.c_str());
            }
            continue;
        }
    }

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

        for (idx_p = if_ni;
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

            string cmd, res;
            /* Bring down the existing kernel interfaces */
            SWSS_LOG_INFO("Bring down old interface %s(%d)", key.c_str(), idx_p->if_index);
            cmd = "ip link set " + key + " down";
            try
            {
                swss::exec(cmd, res);
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
    if ((nlmsg_type != RTM_NEWLINK) && (nlmsg_type != RTM_DELLINK))
    {
        return;
    }

    struct rtnl_link *link = (struct rtnl_link *)obj;
    string key = rtnl_link_get_name(link);

    if (key.compare(0, INTFS_PREFIX.length(), INTFS_PREFIX) &&
        key.compare(0, LAG_PREFIX.length(), LAG_PREFIX) &&
        key.compare(0, MGMT_PREFIX.length(), MGMT_PREFIX))
    {
        return;
    }

    unsigned int flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_LOWER_UP;

    char addrStr[MAX_ADDR_SIZE+1] = {0};
    nl_addr2str(rtnl_link_get_addr(link), addrStr, MAX_ADDR_SIZE);

    unsigned int ifindex = rtnl_link_get_ifindex(link);
    int master = rtnl_link_get_master(link);
    char *type = rtnl_link_get_type(link);

    if (type)
    {
        SWSS_LOG_INFO("nlmsg type:%d key:%s admin:%d oper:%d addr:%s ifindex:%d master:%d type:%s",
                       nlmsg_type, key.c_str(), admin, oper, addrStr, ifindex, master, type);
    }
    else
    {
        SWSS_LOG_INFO("nlmsg type:%d key:%s admin:%d oper:%d addr:%s ifindex:%d master:%d",
                       nlmsg_type, key.c_str(), admin, oper, addrStr, ifindex, master);
    }

    if (!key.compare(0, MGMT_PREFIX.length(), MGMT_PREFIX))
    {
        FieldValueTuple fv("oper_status", oper ? "up" : "down");
        vector<FieldValueTuple> fvs;
        fvs.push_back(fv);
        m_stateMgmtPortTable.set(key, fvs);
        SWSS_LOG_INFO("Store %s oper status %s to state DB",
                key.c_str(), oper ? "up" : "down");
        return;
    }

    /* teamd instances are dealt in teamsyncd */
    if (type && !strcmp(type, TEAM_DRV_NAME))
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

    /* front panel interfaces: Check if the port is in the PORT_TABLE
     * non-front panel interfaces such as eth0, lo which are not in the
     * PORT_TABLE are ignored. */
    vector<FieldValueTuple> temp;
    if (m_portTable.get(key, temp))
    {
        /* TODO: When port is removed from the kernel */
        if (nlmsg_type == RTM_DELLINK)
        {
            return;
        }

        /* Host interface is created */
        if (!g_init && g_portSet.find(key) != g_portSet.end())
        {
            g_portSet.erase(key);
            FieldValueTuple tuple("state", "ok");
            vector<FieldValueTuple> vector;
            vector.push_back(tuple);
            m_statePortTable.set(key, vector);
            SWSS_LOG_INFO("Publish %s(ok) to state db", key.c_str());
        }
    }
}
