#ifndef __LINKSYNC__
#define __LINKSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"

#include <map>

//upscaleai:start
struct nlmsghdr;
//upscaleai:end

namespace swss {

class LinkSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    LinkSync(DBConnector *appl_db, DBConnector *state_db);

    //upscaleai:start
    virtual void onMsg(int nlmsg_type, struct nl_object *obj) override;
    virtual void onMsgRaw(int nlmsg_type, struct nl_object *obj,
                          struct nlmsghdr *nlh) override;
    //upscaleai:end

private:
    //upscaleai:start
    void parseRawLinkAttrs(const std::string &key, struct nlmsghdr *nlh);
    //upscaleai:end

    ProducerStateTable m_portTableProducer;
    Table m_portTable, m_statePortTable;

    std::map<unsigned int, std::string> m_ifindexNameMap;
    std::map<unsigned int, std::string> m_ifindexOldNameMap;
};

}

#endif
