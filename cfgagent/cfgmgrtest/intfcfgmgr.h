#ifndef __INTFCFGMGR__
#define __INTFCFGMGR__

#include "dbconnector.h"
#include "table.h"

namespace swss {

class IntfCfgMgr
{
public:
	enum Operation {
        ADD,
        DELETE,
        SHOW,
    } ;
    enum { MAX_ADDR_SIZE = 64 };

    IntfCfgMgr(DBConnector *db);
    int intf_modify(Operation cmd, int argc, char **argv);
    int intf_show(int argc, char **argv);

private:
    Table m_intfTableProducer;
    Table m_intfTableConsumer;
};

}

#endif
