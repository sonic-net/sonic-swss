#ifndef __INTFSYNC__
#define __INTFSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"

namespace swss {

class IntfSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    IntfSync(DBConnector *db);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

private:
    ProducerStateTable m_intfTable;
};

}

#endif
