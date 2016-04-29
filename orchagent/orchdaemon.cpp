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

    vector<string> qos_tables = {
        APP_TC_TO_QUEUE_MAP_TABLE_NAME,
        APP_SCHEDULER_TABLE_NAME,
        APP_DSCP_TO_TC_MAP_TABLE_NAME,
        APP_QUEUE_TABLE_NAME,
        APP_PORT_QOS_MAP_TABLE_NAME,
        APP_WRED_PROFILE_TABLE_NAME,
        APP_TC_TO_PRIORITY_GROUP_MAP_NAME,
        APP_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_NAME,
        APP_PFC_PRIORITY_TO_QUEUE_MAP_NAME
    };
    QosOrch *qos_orch = new QosOrch(m_applDb, qos_tables, ports_orch);

    vector<string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME,
        APP_BUFFER_PROFILE_TABLE_NAME,
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
    };
    BufferOrch *buffer_orch = new BufferOrch(m_applDb, buffer_tables, ports_orch);

    m_select = new Select();
    m_orchList = { ports_orch, intfs_orch, neigh_orch, route_orch, copp_orch, tunnel_decap_orch, qos_orch, buffer_orch };

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
