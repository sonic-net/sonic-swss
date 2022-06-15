#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"

// The timeout value (in seconds) for neighsyncd reconcilation logic
#define DEFAULT_NEIGHSYNC_WARMSTART_TIMER 5

/*
 * This is the timer value (in seconds) that the neighsyncd waits for restore_neighbors
 * service to finish, should be longer than the restore_neighbors timeout value (110)
 * This should not happen, if happens, system is in a unknown state, we should exit.
 */
#define RESTORE_NEIGH_WAIT_TIME_OUT 180

namespace swss {

class NeighSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb, DBConnector *cfgDb);
    ~NeighSync();

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    bool isNeighRestoreDone();

    AppRestartAssist *getRestartAssist()
    {
        return m_AppRestartAssist;
    }

private:
    Table m_stateNeighRestoreTable;
    ProducerStateTable m_neighTable;
    AppRestartAssist  *m_AppRestartAssist;
    Table m_cfgVlanInterfaceTable, m_cfgLagInterfaceTable, m_cfgInterfaceTable;

    bool isLinkLocalEnabled(const std::string &port);
};

}

#endif
