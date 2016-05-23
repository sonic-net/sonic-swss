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
    {
        delete(m_applDb);
    }

    if (m_asicDb)
    {
        delete(m_asicDb);
    }
    for(Orch* o : m_orch_list)
    {
        delete o;
    }
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
    PortsOrch *portsO = new PortsOrch(m_applDb, ports_tables);
    IntfsOrch *intfsO = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME, portsO);
    NeighOrch *neighO = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, portsO);
    RouteOrch *routeO = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, portsO, neighO);
    std::vector<std::string> qos_tables = {
        APP_TC_TO_QUEUE_MAP_TABLE_NAME, 
        APP_SCHEDULER_TABLE_NAME, 
        APP_DSCP_TO_TC_MAP_TABLE_NAME,
        APP_QUEUE_TABLE_NAME,
        APP_PORT_QOS_MAP_TABLE_NAME,
        APP_WRED_PROFILE_TABLE_NAME
        };
    QosOrch *qosO = new QosOrch(m_applDb, qos_tables, portsO);
    std::vector<std::string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME, 
        APP_BUFFER_PROFILE_TABLE_NAME, 
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
        };
    BufferOrch *bufferO = new BufferOrch(m_applDb, buffer_tables, portsO);
    m_select = new Select();
    m_orch_list = {portsO, intfsO, neighO, routeO, qosO, bufferO};
    return true;
}

void OrchDaemon::start()
{
    SWSS_LOG_ENTER();
    for(Orch* o : m_orch_list)
    {
        m_select->addSelectables(o->getConsumers());
    }    
    while (true)
    {
        Selectable *s;
        int fd;
        int ret = m_select->select(&s, &fd, 1);
        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
            continue;
        }
        if (ret == Select::TIMEOUT)
        {            
            /*  Run througs all orchs and drain their toSync caches
                It's possible that there will be no items coming from DB,
                but there can be items remainins in the DB which need to be re-tried.
                For example if given entry's dependency isn't satisfied, we need to retry
                executing it in future.
                Donwnside is - periodic retry for items which never get their dependencies satisfied.
            */
            for(Orch* o : m_orch_list)
            {
                std::vector<Selectable*> consumers = o->getConsumers();
                for(Selectable* sl : consumers)
                {
                    o->execute(((ConsumerTable *)sl)->getTableName(), false);
                }
            }
        }
        else
        {
            Orch *o = getOrchByConsumer((ConsumerTable *)s);
            o->execute(((ConsumerTable *)s)->getTableName(), true);
        }
    }
}

Orch *OrchDaemon::getOrchByConsumer(ConsumerTable *c)
{
    SWSS_LOG_ENTER();
    for(Orch* o : m_orch_list)
    {
        if(o->hasConsumer(c))
        {
            return o;
        }
    }    
    return nullptr;
}
