#include <string.h>
#include <errno.h>
#include <system_error>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producertable.h"
#include "linkcache.h"
#include "neighsyncd/neighsync.h"

using namespace std;
using namespace swss;

NeighSync::NeighSync(DBConnector *db) :
    m_neighTable(db, APP_NEIGH_TABLE_NAME)
{
}

void NeighSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char ipStr[MAX_ADDR_SIZE + 1] = {0};
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    string key;
    string family;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    if (rtnl_neigh_get_family(neigh) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_neigh_get_family(neigh) == AF_INET6)
        family = IPV6_NAME;
    else
        return;

    key+= LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));
    key+= ":";

    nl_addr2str(rtnl_neigh_get_dst(neigh), ipStr, MAX_ADDR_SIZE);
    string ip(ipStr);
    if (family == IPV6_NAME && ip.compare(0, 4, "ff02"))
        return;
    key+= ipStr;

    int state = rtnl_neigh_get_state(neigh);
    if ((nlmsg_type == RTM_DELNEIGH) || (state == NUD_INCOMPLETE) ||
        (state == NUD_FAILED))
    {
        m_neighTable.del(key);
        return;
    }

    nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);
    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple nh("neigh", macStr);
    fvVector.push_back(nh);
    fvVector.push_back(f);
    m_neighTable.set(key, fvVector);
}
