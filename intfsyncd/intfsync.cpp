#include <string.h>
#include <errno.h>
#include <system_error>
#include <netlink/route/link.h>
#include <netlink/route/addr.h>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producertable.h"
#include "scheme.h"
#include "linkcache.h"
#include "intfsyncd/intfsync.h"

using namespace std;
using namespace swss;

IntfSync::IntfSync(DBConnector *db) :
    m_intfTable(db, APP_INTF_TABLE_NAME)
{
}

void IntfSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char addrStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_addr *addr = (struct rtnl_addr *)obj;
    string key;
    string scope = "global";
    string family;

    if ((nlmsg_type != RTM_NEWADDR) && (nlmsg_type != RTM_GETADDR) &&
        (nlmsg_type != RTM_DELADDR))
        return;

    /* Don't sync local routes */
    if (rtnl_addr_get_scope(addr) != RT_SCOPE_UNIVERSE)
    {
        scope = "local";
        return;
    }

    if (rtnl_addr_get_family(addr) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_addr_get_family(addr) == AF_INET6)
        family = IPV6_NAME;
    else
        // Not supported
        return;

    key = LinkCache::getInstance().ifindexToName(rtnl_addr_get_ifindex(addr));
    key+= ":";
    nl_addr2str(rtnl_addr_get_local(addr), addrStr, MAX_ADDR_SIZE);
    key+= addrStr;
    if (nlmsg_type == RTM_DELADDR)
    {
        m_intfTable.del(key);
        return;
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple s("scope", scope);
    fvVector.push_back(s);
    fvVector.push_back(f);
    m_intfTable.set(key, fvVector);
}
