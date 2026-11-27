#include "intfmgr_netlink.h"
#include "intfmgr.h"
#include "logger.h"

#include <linux/rtnetlink.h>
#include <netlink/route/addr.h>

using namespace std;
using namespace swss;

IntfAddrNetlink::IntfAddrNetlink(IntfMgr *intfmgr) :
    m_intfmgr(intfmgr)
{
}

void IntfAddrNetlink::onMsg(int nlmsg_type, struct nl_object *obj)
{
    if (m_intfmgr == nullptr)
    {
        return;
    }

    if (nlmsg_type != RTM_NEWADDR && nlmsg_type != RTM_DELADDR)
    {
        return;
    }

    m_intfmgr->handleNetlinkAddr(nlmsg_type, obj);
}
