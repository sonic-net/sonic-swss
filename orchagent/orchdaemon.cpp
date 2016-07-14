#include "orchdaemon.h"

#include "logger.h"

#include <unistd.h>

using namespace std;
using namespace swss;

OrchDaemon::OrchDaemon()
{
    m_applDb = nullptr;
    m_asicDb = nullptr;
}

OrchDaemon::~OrchDaemon()
{
    if (m_applDb)
        delete(m_applDb);

    if (m_asicDb)
        delete(m_asicDb);

    for (Orch *o : m_orchList)
        delete(o);
}

bool OrchDaemon::init()
{
    SWSS_LOG_ENTER();

    m_applDb = new DBConnector(APPL_DB, "localhost", 6379, 0);

    vector<string> ports_tables = {
        APP_PORT_TABLE_NAME,
        APP_VLAN_TABLE_NAME,
        APP_LAG_TABLE_NAME
    };

    PortsOrch *ports_orch = new PortsOrch(m_applDb, ports_tables);
    IntfsOrch *intfs_orch = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME, ports_orch);
    NeighOrch *neigh_orch = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, ports_orch);
    RouteOrch *route_orch = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, ports_orch, neigh_orch);
    CoppOrch  *copp_orch  = new CoppOrch(m_applDb, APP_COPP_TABLE_NAME);
    TunnelDecapOrch *tunnel_decap_orch = new TunnelDecapOrch(m_applDb, APP_TUNNEL_DECAP_TABLE_NAME);
    
    m_orchList = { ports_orch, intfs_orch, neigh_orch, route_orch, copp_orch, tunnel_decap_orch };
    m_select = new Select();

    return true;
}

void OrchDaemon::start()
{
    SWSS_LOG_ENTER();

    for (Orch *o : m_orchList)
    {
        m_select->addSelectables(o->getSelectables());
    }

    while (true)
    {
        Selectable *s;
        int fd, ret;

        ret = m_select->select(&s, &fd, 1);
        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
            continue;
        }

        if (ret == Select::TIMEOUT)
        {
            /* After every TIMEOUT, periodically check all m_toSync map to
             * execute all the remaining tasks that need to be retried. */
            for (Orch *o : m_orchList)
                o->doTask();

            continue;
        }

        Orch *o = getOrchByConsumer((ConsumerTable *)s);
        o->execute(((ConsumerTable *)s)->getTableName());
    }
}

Orch *OrchDaemon::getOrchByConsumer(ConsumerTable *c)
{
    SWSS_LOG_ENTER();

    for (Orch *o : m_orchList)
    {
        if (o->hasSelectable(c))
            return o;
    }

    SWSS_LOG_ERROR("Failed to get Orch class by ConsumerTable:%s",
            c->getTableName().c_str());

    return nullptr;
}
