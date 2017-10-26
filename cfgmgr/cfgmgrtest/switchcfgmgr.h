#ifndef __SWITCHCFGMGR__
#define __SWITCHCFGMGR__

#include "dbconnector.h"
#include "table.h"

namespace swss {

class SwitchCfgMgr
{
public:
    enum Operation {
        ADD,
        DELETE,
        SHOW,
    } ;
    enum { MAX_ADDR_SIZE = 64 };

    SwitchCfgMgr(DBConnector *db);
    int switch_update(int argc, char **argv);
    int switch_show(int argc, char **argv);

private:
    Table m_cfgSwitchTable;
};

}

#endif
