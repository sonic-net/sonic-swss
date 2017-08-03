#include <unistd.h>
#include "orchdaemon.h"
#include "logger.h"
#include "sairedis.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time */
#define SELECT_TIMEOUT 1000

extern sai_switch_api_t*           sai_switch_api;

/* Global variable gPortsOrch declared */
PortsOrch *gPortsOrch;
/* Global variable gFdbOrch declared */
FdbOrch *gFdbOrch;

OrchDaemon::OrchDaemon(DBConnector *applDb) :
        m_applDb(applDb)
{
    SWSS_LOG_ENTER();
}

OrchDaemon::~OrchDaemon()
{
    SWSS_LOG_ENTER();
    for (Orch *o : m_orchList)
        delete(o);
}

bool OrchDaemon::init()
{
    SWSS_LOG_ENTER();

    vector<string> ports_tables = {
        APP_PORT_TABLE_NAME,
        APP_VLAN_TABLE_NAME,
        APP_VLAN_MEMBER_TABLE_NAME,
        APP_LAG_TABLE_NAME,
        APP_LAG_MEMBER_TABLE_NAME
    };

    gPortsOrch = new PortsOrch(m_applDb, ports_tables);
    gFdbOrch = new FdbOrch(m_applDb, APP_FDB_TABLE_NAME, gPortsOrch);
    IntfsOrch *intfs_orch = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME);
    NeighOrch *neigh_orch = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, intfs_orch);
    RouteOrch *route_orch = new RouteOrch(m_applDb, APP_ROUTE_TABLE_NAME, neigh_orch);
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
    QosOrch *qos_orch = new QosOrch(m_applDb, qos_tables);

    vector<string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME,
        APP_BUFFER_PROFILE_TABLE_NAME,
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_CONFIG_TO_PG_PROFILE_TABLE,
        APP_BUFFER_PORT_CABLE_LENGTH_TABLE
    };
    BufferOrch *buffer_orch = new BufferOrch(m_applDb, buffer_tables, gPortsOrch);

    MirrorOrch *mirror_orch = new MirrorOrch(m_applDb, APP_MIRROR_SESSION_TABLE_NAME, gPortsOrch, route_orch, neigh_orch, gFdbOrch);

    vector<string> acl_tables = {
        APP_ACL_TABLE_NAME,
        APP_ACL_RULE_TABLE_NAME
    };
    AclOrch *acl_orch = new AclOrch(m_applDb, acl_tables, gPortsOrch, mirror_orch, neigh_orch, route_orch);

    m_orchList = { gPortsOrch, intfs_orch, neigh_orch, route_orch, copp_orch, tunnel_decap_orch, qos_orch, buffer_orch, mirror_orch, acl_orch, gFdbOrch};
    m_select = new Select();

    return true;
}

/* Flush redis through sairedis interface */
void OrchDaemon::flush()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_FLUSH;
    sai_status_t status = sai_switch_api->set_switch_attribute(&attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to flush redis pipeline %d", status);
        exit(EXIT_FAILURE);
    }
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

        ret = m_select->select(&s, &fd, SELECT_TIMEOUT);

        if (ret == Select::ERROR)
        {
            SWSS_LOG_NOTICE("Error: %s!\n", strerror(errno));
            continue;
        }

        if (ret == Select::TIMEOUT)
        {
            /* Let sairedis to flush all SAI function call to ASIC DB.
             * Normally the redis pipeline will flush when enough request
             * accumulated. Still it is possible that small amount of
             * requests live in it. When the daemon has nothing to do, it
             * is a good chance to flush the pipeline  */
            flush();
            continue;
        }

        Orch *o = getOrchByConsumer((ConsumerStateTable *)s);
        o->execute(((ConsumerStateTable *)s)->getTableName());

        /* After each iteration, periodically check all m_toSync map to
         * execute all the remaining tasks that need to be retried. */

        /* TODO: Abstract Orch class to have a specific todo list */
        for (Orch *o : m_orchList)
            o->doTask();

    }
}

Orch *OrchDaemon::getOrchByConsumer(ConsumerStateTable *c)
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
