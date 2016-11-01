#ifndef __LINKSYNC__
#define __LINKSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"

#include <map>

namespace swss {

class LinkSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    LinkSync(DBConnector *db);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

private:
    ProducerStateTable m_portTableProducer, m_vlanTableProducer, m_lagTableProducer;
    Table m_portTableConsumer, m_vlanTableConsumer, m_lagTableConsumer;

    std::map<unsigned int, std::string> m_ifindexNameMap;
};

}

#endif
