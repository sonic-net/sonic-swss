#ifndef __LINKSYNC__
#define __LINKSYNC__

#include "common/dbconnector.h"
#include "common/producertable.h"
#include "common/netmsg.h"

namespace swss {

class LinkSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    LinkSync(DBConnector *db);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

private:
    ProducerTable m_portTableProducer, m_vlanTableProducer, m_lagTableProducer;
    Table m_portTableConsumer, m_vlanTableConsumer, m_lagTableConsumer;
};

}

#endif
