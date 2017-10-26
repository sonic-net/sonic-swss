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

#include "linkcache.h"
#include "portsyncd/linksync.h"

#include <iostream>
#include <set>

using namespace std;
using namespace swss;

#define VLAN_DRV_NAME   "bridge"
#define TEAM_DRV_NAME   "team"

const string INTFS_PREFIX = "Ethernet";
const string VLAN_PREFIX = "Vlan";
const string LAG_PREFIX = "PortChannel";

extern set<string> g_portSet;
extern map<string, set<string>> g_vlanMap;
extern bool g_init;

LinkSync::LinkSync(DBConnector *appl_db, DBConnector *state_db) :
    m_portTableProducer(appl_db, APP_PORT_TABLE_NAME),
    m_portTable(appl_db, APP_PORT_TABLE_NAME),
    m_statePortTable(state_db, STATE_PORT_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR)
{
    /* See the comments for g_portSet in portsyncd.cpp */
    for (string port : g_portSet)
    {
        vector<FieldValueTuple> temp;
        if (m_portTable.get(port, temp))
        {
            for (auto it : temp)
            {
                if (fvField(it) == "admin_status")
                {
                    g_portSet.erase(port);
                    break;
                }
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
        key.compare(0, LAG_PREFIX.length(), LAG_PREFIX))
    {
        return;
    }

    unsigned int flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_LOWER_UP;
    unsigned int mtu = rtnl_link_get_mtu(link);

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

    /* Insert or update the ifindex to key map */
    m_ifindexNameMap[ifindex] = key;

    /* teamd instances are dealt in teamsyncd */
    if (type && !strcmp(type, TEAM_DRV_NAME))
    {
        return;
    }

    vector<FieldValueTuple> fvVector;
    FieldValueTuple a("admin_status", admin ? "up" : "down");
    FieldValueTuple m("mtu", to_string(mtu));
    fvVector.push_back(a);
    fvVector.push_back(m);

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
        }

        m_portTableProducer.set(key, fvVector);
    }
}
