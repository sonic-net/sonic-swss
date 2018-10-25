#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"

#define DEFAULT_NEIGHSYNC_WARMSTART_TIMER 5

namespace swss {

class NeighSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    bool isNeighRestoreDone();

    AppRestartAssist *getRestartAssist()
    {
        return &m_AppRestartAssist;
    }

private:
    Table m_stateNeighRestoreTable;
    ProducerStateTable m_neighTable;
    AppRestartAssist m_AppRestartAssist;
};

}

#endif
