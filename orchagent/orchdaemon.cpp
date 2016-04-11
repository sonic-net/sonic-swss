#include "orchdaemon.h"
#include "routeorch.h"
#include "neighorch.h"

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
    m_applDb = new DBConnector(APPL_DB, "localhost", 6379, 0);

    m_portsO = new PortsOrch(m_applDb, APP_PORT_TABLE_NAME);
    m_intfsO = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME, m_portsO);
    m_routeO = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, m_portsO);
    m_neighO = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, m_portsO, m_routeO);
    std::vector<std::string> qos_tables = {
        APP_TC_TO_QUEUE_MAP_TABLE_NAME, 
        APP_SCHEDULER_TABLE_NAME, 
        APP_DSCP_TO_TC_MAP_TABLE_NAME,
        APP_QUEUE_TABLE_NAME,
        APP_PORT_QOS_MAP_TABLE_NAME,
        APP_WRED_PROFILE_TABLE_NAME
        };
    
    m_qosO   = new QosOrch(m_applDb, qos_tables, m_portsO);
    m_select = new Select();

    return true;
}

void OrchDaemon::start()
{
    int ret;
    std::vector<Selectable *> selectables;
    
    m_portsO->getSelectables(selectables);
    m_select->addSelectables(selectables);
    
    m_intfsO->getSelectables(selectables);
    m_select->addSelectables(selectables);
    
    m_neighO->getSelectables(selectables);
    m_select->addSelectables(selectables);
    
    m_routeO->getSelectables(selectables);
    m_select->addSelectables(selectables);

    m_qosO->getSelectables(selectables);
    m_select->addSelectables(selectables);

    while (true)
    {
        Selectable *s;
        int fd;

        ret = m_select->select(&s, &fd, 1);
        if (ret == Select::ERROR)
            SWSS_LOG_ERROR("Failed to obtain selectable\n");

        if (ret == Select::TIMEOUT)
            continue;

        Orch *o = getOrchByConsumer((ConsumerTable *)s);// NOTE: o can be nullptr

        SWSS_LOG_INFO("Get message from Orch: %s\n", o->getOrchName().c_str());
        o->execute(((ConsumerTable *)s)->getTableName());
    }
}

Orch *OrchDaemon::getOrchByConsumer(ConsumerTable *c)
{
    if (m_portsO->is_owned_consumer(c))
        return m_portsO;
    if (m_intfsO->is_owned_consumer(c))
        return m_intfsO;
    if (m_neighO->is_owned_consumer(c))
        return m_neighO;
    if (m_routeO->is_owned_consumer(c))
        return m_routeO;
    return nullptr;
}
