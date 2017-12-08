#include <unistd.h>
#include "orchdaemon.h"
#include "logger.h"
#include <sairedis.h>

#define SAI_SWITCH_ATTR_CUSTOM_RANGE_BASE SAI_SWITCH_ATTR_CUSTOM_RANGE_START
#include "sairedis.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time */
#define SELECT_TIMEOUT 1000
#define FLEX_COUNTER_POLL_MSECS 100

extern sai_switch_api_t*           sai_switch_api;
extern sai_object_id_t             gSwitchId;

/* Global variable gPortsOrch declared */
PortsOrch *gPortsOrch;
/* Global variable gFdbOrch declared */
FdbOrch *gFdbOrch;
/*Global variable gAclOrch declared*/
AclOrch *gAclOrch;

OrchDaemon::OrchDaemon(DBConnector *applDb, DBConnector *configDb) :
        m_applDb(applDb),
        m_configDb(configDb)

{
    SWSS_LOG_ENTER();
}

OrchDaemon::~OrchDaemon()
{
    SWSS_LOG_ENTER();
    for (Orch *o : m_orchList)
        delete(o);

    delete(m_configDb);
    delete(m_applDb);
}

bool OrchDaemon::init()
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";

    SwitchOrch *switch_orch = new SwitchOrch(m_applDb, APP_SWITCH_TABLE_NAME);

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
//    TunnelDecapOrch *tunnel_decap_orch = new TunnelDecapOrch(m_applDb, APP_TUNNEL_DECAP_TABLE_NAME);

    vector<string> qos_tables = {
        CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
        CFG_SCHEDULER_TABLE_NAME,
        CFG_DSCP_TO_TC_MAP_TABLE_NAME,
        CFG_QUEUE_TABLE_NAME,
        CFG_PORT_QOS_MAP_TABLE_NAME,
        CFG_WRED_PROFILE_TABLE_NAME,
        CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
        CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
        CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME
    };
    QosOrch *qos_orch = new QosOrch(m_configDb, qos_tables);

    vector<string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME,
        APP_BUFFER_PROFILE_TABLE_NAME,
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
    };
    BufferOrch *buffer_orch = new BufferOrch(m_applDb, buffer_tables);

    TableConnector appDbMirrorSession(m_applDb, APP_MIRROR_SESSION_TABLE_NAME);
    TableConnector confDbMirrorSession(m_configDb, CFG_MIRROR_SESSION_TABLE_NAME);
    MirrorOrch *mirror_orch = new MirrorOrch(appDbMirrorSession, confDbMirrorSession, gPortsOrch, route_orch, neigh_orch, gFdbOrch);

    vector<string> acl_tables = {
        CFG_ACL_TABLE_NAME,
        CFG_ACL_RULE_TABLE_NAME
    };
    gAclOrch = new AclOrch(m_configDb, acl_tables, gPortsOrch, mirror_orch, neigh_orch, route_orch);

    vector<string> dtel_tables = {
        APP_DTEL_TABLE_NAME,
        APP_DTEL_REPORT_SESSION_TABLE_NAME,
        APP_DTEL_INT_SESSION_TABLE_NAME,
        APP_DTEL_QUEUE_REPORT_TABLE_NAME,
        APP_DTEL_EVENT_TABLE_NAME
    };

    DTelOrch *dtel_orch = new DTelOrch(m_applDb, dtel_tables, gPortsOrch);

    m_orchList = { switch_orch, gPortsOrch, intfs_orch, neigh_orch, route_orch, copp_orch, /*tunnel_decap_orch,*/ qos_orch, buffer_orch, mirror_orch, gAclOrch, gFdbOrch, dtel_orch};
    m_select = new Select();

    vector<string> pfc_wd_tables = {
        APP_PFC_WD_TABLE_NAME
    };

    if (platform == MLNX_PLATFORM_SUBSTRING)
    {

        static const vector<sai_port_stat_t> portStatIds =
        {
            SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION,
            SAI_PORT_STAT_PFC_0_RX_PKTS,
            SAI_PORT_STAT_PFC_1_RX_PKTS,
            SAI_PORT_STAT_PFC_2_RX_PKTS,
            SAI_PORT_STAT_PFC_3_RX_PKTS,
            SAI_PORT_STAT_PFC_4_RX_PKTS,
            SAI_PORT_STAT_PFC_5_RX_PKTS,
            SAI_PORT_STAT_PFC_6_RX_PKTS,
            SAI_PORT_STAT_PFC_7_RX_PKTS,
        };

        static const vector<sai_queue_stat_t> queueStatIds =
        {
            SAI_QUEUE_STAT_PACKETS,
            SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
        };

        static const vector<sai_queue_attr_t> queueAttrIds;

        m_orchList.push_back(new PfcWdSwOrch<PfcWdZeroBufferHandler, PfcWdLossyHandler>(
                    m_configDb,
                    pfc_wd_tables,
                    portStatIds,
                    queueStatIds,
                    queueAttrIds,
                    FLEX_COUNTER_POLL_MSECS));
    }
    else if (platform == BRCM_PLATFORM_SUBSTRING)
    {
        static const vector<sai_port_stat_t> portStatIds =
        {
            SAI_PORT_STAT_PFC_0_RX_PKTS,
            SAI_PORT_STAT_PFC_1_RX_PKTS,
            SAI_PORT_STAT_PFC_2_RX_PKTS,
            SAI_PORT_STAT_PFC_3_RX_PKTS,
            SAI_PORT_STAT_PFC_4_RX_PKTS,
            SAI_PORT_STAT_PFC_5_RX_PKTS,
            SAI_PORT_STAT_PFC_6_RX_PKTS,
            SAI_PORT_STAT_PFC_7_RX_PKTS,
            SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS,
            SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS,
        };

        static const vector<sai_queue_stat_t> queueStatIds =
        {
            SAI_QUEUE_STAT_PACKETS,
            SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
        };

        static const vector<sai_queue_attr_t> queueAttrIds =
        {
            SAI_QUEUE_ATTR_PAUSE_STATUS,
        };

        m_orchList.push_back(new PfcWdSwOrch<PfcWdActionHandler, PfcWdActionHandler>(
                    m_configDb,
                    pfc_wd_tables,
                    portStatIds,
                    queueStatIds,
                    queueAttrIds,
                    FLEX_COUNTER_POLL_MSECS));
    }

    return true;
}

/* Flush redis through sairedis interface */
void OrchDaemon::flush()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_FLUSH;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
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

        auto *c = (Executor *)s;
        c->execute();

        /* After each iteration, periodically check all m_toSync map to
         * execute all the remaining tasks that need to be retried. */

        /* TODO: Abstract Orch class to have a specific todo list */
        for (Orch *o : m_orchList)
            o->doTask();

    }
}
