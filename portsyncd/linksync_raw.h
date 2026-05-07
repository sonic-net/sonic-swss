//upscaleai:start
#ifndef __LINKSYNC_RAW__
#define __LINKSYNC_RAW__

#include "portsyncd/linksync.h"
#include "table.h"

struct nlmsghdr;

namespace swss {

class LinkSyncRaw : public LinkSync
{
public:
    LinkSyncRaw(DBConnector *appl_db, DBConnector *state_db);

    void onMsg(int nlmsg_type, struct nl_object *obj) override;

private:
    void parseRawLinkAttrs(const std::string &key, struct nlmsghdr *nlh);
    Table m_statePortTableRaw;
};

}

#endif
//upscaleai:end
