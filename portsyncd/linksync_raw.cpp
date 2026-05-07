//upscaleai:start
#include "linksync_raw.h"
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <netlink/route/link.h>
#include <net/if.h>
#include "logger.h"
#include "netmsg_raw.h"

/*
 * Kernel IFLA attributes not exposed by the installed libnl3.
 * Values are stable UAPI (include/uapi/linux/if_link.h).
 */
#ifndef IFLA_PROTO_DOWN
#define IFLA_PROTO_DOWN 39
#endif
#ifndef IFLA_CARRIER_UP_COUNT
#define IFLA_CARRIER_UP_COUNT 47
#endif
#ifndef IFLA_CARRIER_DOWN_COUNT
#define IFLA_CARRIER_DOWN_COUNT 48
#endif
#ifndef IFLA_PERM_ADDRESS
#define IFLA_PERM_ADDRESS 54
#endif

/* ifinfomsg is the fixed header after nlmsghdr in RTM_*LINK messages. */
struct ifinfomsg_fixed {
    unsigned char  ifi_family;
    unsigned char  __ifi_pad;
    unsigned short ifi_type;
    int            ifi_index;
    unsigned int   ifi_flags;
    unsigned int   ifi_change;
};

using namespace std;
using namespace swss;

extern const string INTFS_PREFIX;
extern const string LAG_PREFIX;

LinkSyncRaw::LinkSyncRaw(DBConnector *appl_db, DBConnector *state_db) :
    LinkSync(appl_db, state_db),
    m_statePortTableRaw(state_db, STATE_PORT_TABLE_NAME)
{
}

void LinkSyncRaw::onMsg(int nlmsg_type, struct nl_object *obj)
{
    /* Upstream logic runs first */
    LinkSync::onMsg(nlmsg_type, obj);

    /* Augment with raw attributes (read from thread-local stash) */
    if (nlmsg_type != RTM_NEWLINK)
    {
        return;
    }

    struct nlmsghdr *nlh = RawNetMsg::s_current_nlh;
    if (!nlh)
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

    parseRawLinkAttrs(key, nlh);
}

void LinkSyncRaw::parseRawLinkAttrs(const string &key, struct nlmsghdr *nlh)
{
    struct nlattr *tb[IFLA_PERM_ADDRESS + 1] = {};

    if (nlmsg_parse(nlh, sizeof(ifinfomsg_fixed), tb, IFLA_PERM_ADDRESS, NULL) < 0)
    {
        SWSS_LOG_WARN("Failed to parse raw link attrs for %s", key.c_str());
        return;
    }

    vector<FieldValueTuple> fvs;

    if (tb[IFLA_CARRIER_UP_COUNT])
    {
        fvs.emplace_back("carrier_up_count", to_string(nla_get_u32(tb[IFLA_CARRIER_UP_COUNT])));
    }

    if (tb[IFLA_CARRIER_DOWN_COUNT])
    {
        fvs.emplace_back("carrier_down_count", to_string(nla_get_u32(tb[IFLA_CARRIER_DOWN_COUNT])));
    }

    if (tb[IFLA_PROTO_DOWN])
    {
        fvs.emplace_back("protodown", nla_get_u8(tb[IFLA_PROTO_DOWN]) ? "true" : "false");
    }

    if (tb[IFLA_PERM_ADDRESS])
    {
        int len = nla_len(tb[IFLA_PERM_ADDRESS]);
        unsigned char *addr = (unsigned char *)nla_data(tb[IFLA_PERM_ADDRESS]);
        if (len >= 6)
        {
            char buf[18];
            snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            fvs.emplace_back("perm_hw_addr", buf);
        }
    }

    if (!fvs.empty())
    {
        m_statePortTableRaw.set(key, fvs);
        SWSS_LOG_INFO("Published raw link attrs for %s", key.c_str());
    }
}
//upscaleai:end
