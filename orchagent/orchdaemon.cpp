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

    m_portsO = new PortsOrch(m_applDb, ports_tables);
    m_intfsO = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME, m_portsO);
    m_neighO = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, m_portsO);
    m_routeO = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, m_portsO, m_neighO);

    m_select = new Select();

    return true;
}

bool OrchDaemon::reinit()
{
    SWSS_LOG_ENTER();

    m_portsO->reinit();
    m_intfsO->reinit();
    m_neighO->reinit();
    m_routeO->reinit();

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
    return nullptr;
}
