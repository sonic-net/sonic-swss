#include "orchdaemon.h"
#include "routeorch.h"
#include "neighorch.h"

#include "logger.h"

#include <unistd.h>

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

    m_select = new Select();

    return true;
}

void OrchDaemon::start()
{
    int ret;

    m_select->addSelectable(m_portsO->getConsumer());
    m_select->addSelectable(m_intfsO->getConsumer());
    m_select->addSelectable(m_neighO->getConsumer());
    m_select->addSelectable(m_routeO->getConsumer());

    while (true)
    {
        Selectable *s;
        int fd;

        ret = m_select->select(&s, &fd, 1);
        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error!\n");
            continue;
        }

        if (ret == Select::TIMEOUT)
            continue;

        Orch *o = getOrchByConsumer((ConsumerTable *)s);

        SWSS_LOG_INFO("Get message from Orch: %s\n", o->getOrchName().c_str());
        o->execute();
    }
}

Orch *OrchDaemon::getOrchByConsumer(ConsumerTable *c)
{
    if (m_portsO->getConsumer() == c)
        return m_portsO;
    if (m_intfsO->getConsumer() == c)
        return m_intfsO;
    if (m_neighO->getConsumer() == c)
        return m_neighO;
    if (m_routeO->getConsumer() == c)
        return m_routeO;
    return nullptr;
}
