#include "orchdaemon.h"

#include "logger.h"

#include <unistd.h>

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
}

bool OrchDaemon::init()
{
    SWSS_LOG_ENTER();

    m_applDb = new DBConnector(APPL_DB, "localhost", 6379, 0);

    m_portsO = new PortsOrch(m_applDb, APP_PORT_TABLE_NAME);
    m_intfsO = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME, m_portsO);
    m_neighO = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, m_portsO);
    m_routeO = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, m_portsO, m_neighO);
    std::vector<std::string> qos_tables = {
        APP_TC_TO_QUEUE_MAP_TABLE_NAME, 
        APP_SCHEDULER_TABLE_NAME, 
        APP_DSCP_TO_TC_MAP_TABLE_NAME,
        APP_QUEUE_TABLE_NAME,
        APP_PORT_QOS_MAP_TABLE_NAME,
        APP_WRED_PROFILE_TABLE_NAME
        };
    m_qosO = new QosOrch(m_applDb, qos_tables, m_portsO);
    std::vector<std::string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME, 
        APP_BUFFER_PROFILE_TABLE_NAME, 
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
        };
    m_bufferO = new BufferOrch(m_applDb, buffer_tables, m_portsO);
    m_select = new Select();

    return true;
}

void OrchDaemon::start()
{
    SWSS_LOG_ENTER();

    int ret;
    m_select->addSelectables(m_portsO->getConsumers());
    m_select->addSelectables(m_intfsO->getConsumers());
    m_select->addSelectables(m_neighO->getConsumers());
    m_select->addSelectables(m_routeO->getConsumers());
    m_select->addSelectables(m_qosO->getConsumers());
    m_select->addSelectables(m_bufferO->getConsumers());

    while (true)
    {
        Selectable *s;
        int fd;

        ret = m_select->select(&s, &fd, 1);
        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
            continue;
        }

        if (ret == Select::TIMEOUT)
            continue;

        Orch *o = getOrchByConsumer((ConsumerTable *)s);
        o->execute(((ConsumerTable *)s)->getTableName());
    }
}

Orch *OrchDaemon::getOrchByConsumer(ConsumerTable *c)
{
    SWSS_LOG_ENTER();

    if (m_portsO->hasConsumer(c))
        return m_portsO;
    if (m_intfsO->hasConsumer(c))
        return m_intfsO;
    if (m_neighO->hasConsumer(c))
        return m_neighO;
    if (m_routeO->hasConsumer(c))
        return m_routeO;
    if (m_qosO->hasConsumer(c))
        return m_qosO;
    if (m_bufferO->hasConsumer(c))
        return m_bufferO;
    return nullptr;
}
