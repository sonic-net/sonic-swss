#ifndef __INTFMGR_NETLINK__
#define __INTFMGR_NETLINK__

#include "netmsg.h"

namespace swss {

class IntfMgr;

class IntfAddrNetlink : public NetMsg
{
public:
    explicit IntfAddrNetlink(IntfMgr *intfmgr);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj) override;

private:
    IntfMgr *m_intfmgr;
};

}

#endif
