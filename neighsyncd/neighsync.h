#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"

#define DEFAULT_NEIGHSYNC_WARMSTART_TIMER 5

//This is the timer value (in seconds) that the neighsyncd waiting for restore_neighbors
//service to finish, should be longer than the restore_neighbors timeout value (60)
//This should not happen, if happens, system is in a unknown state, we should exit.
#define RESTORE_NEIGH_WAIT_TIME_OUT 70

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
