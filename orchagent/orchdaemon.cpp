#include <unistd.h>
#include <unordered_map>
#include <chrono>
#include <limits.h>
#include "orchdaemon.h"
#include "logger.h"
#include <sairedis.h>
#include "warm_restart.h"
#include <iostream>

#define SAI_SWITCH_ATTR_CUSTOM_RANGE_BASE SAI_SWITCH_ATTR_CUSTOM_RANGE_START
#include "sairedis.h"
#include "chassisorch.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time */
#define SELECT_TIMEOUT 1000
#define PFC_WD_POLL_MSECS 100

#define APP_FABRIC_MONITOR_PORT_TABLE_NAME      "FABRIC_PORT_TABLE"
#define APP_FABRIC_MONITOR_DATA_TABLE_NAME      "FABRIC_MONITOR_TABLE"

/* orchagent heart beat message interval */
#define HEART_BEAT_INTERVAL_MSECS 10 * 1000

extern sai_switch_api_t*           sai_switch_api;
extern sai_object_id_t             gSwitchId;
extern string                      gMySwitchType;

extern void syncd_apply_view();
/*
 * Global orch daemon variables
 */
PortsOrch *gPortsOrch;
FabricPortsOrch *gFabricPortsOrch;
FdbOrch *gFdbOrch;
IntfsOrch *gIntfsOrch;
NeighOrch *gNeighOrch;
RouteOrch *gRouteOrch;
NhgOrch *gNhgOrch;
NhgMapOrch *gNhgMapOrch;
CbfNhgOrch *gCbfNhgOrch;
FgNhgOrch *gFgNhgOrch;
AclOrch *gAclOrch;
PbhOrch *gPbhOrch;
MirrorOrch *gMirrorOrch;
CrmOrch *gCrmOrch;
BufferOrch *gBufferOrch;
QosOrch *gQosOrch;
SwitchOrch *gSwitchOrch;
Directory<Orch*> gDirectory;
NatOrch *gNatOrch;
PolicerOrch *gPolicerOrch;
MlagOrch *gMlagOrch;
IsoGrpOrch *gIsoGrpOrch;
MACsecOrch *gMacsecOrch;
CoppOrch *gCoppOrch;
P4Orch *gP4Orch;
BfdOrch *gBfdOrch;
Srv6Orch *gSrv6Orch;
FlowCounterRouteOrch *gFlowCounterRouteOrch;
DebugCounterOrch *gDebugCounterOrch;
MonitorOrch *gMonitorOrch;
TunnelDecapOrch *gTunneldecapOrch;

bool gIsNatSupported = false;
event_handle_t g_events_handle;

#define DEFAULT_MAX_BULK_SIZE 1000
size_t gMaxBulkSize = DEFAULT_MAX_BULK_SIZE;

OrchDaemon::OrchDaemon(DBConnector *applDb, DBConnector *configDb, DBConnector *stateDb, DBConnector *chassisAppDb, ZmqServer *zmqServer) :
        m_applDb(applDb),
        m_configDb(configDb),
        m_stateDb(stateDb),
        m_chassisAppDb(chassisAppDb),
        m_zmqServer(zmqServer)
{
    SWSS_LOG_ENTER();
    m_select = new Select();
    m_lastHeartBeat = std::chrono::high_resolution_clock::now();
}

OrchDaemon::~OrchDaemon()
{
    SWSS_LOG_ENTER();

    /*
     * Some orchagents call other agents in their destructor.
     * To avoid accessing deleted agent, do deletion in reverse order.
     * NOTE: This is still not a robust solution, as order in this list
     *       does not strictly match the order of construction of agents.
     * For a robust solution, first some cleaning/house-keeping in
     * orchagents management is in order.
     * For now it fixes, possible crash during process exit.
     */
    auto it = m_orchList.rbegin();
    for(; it != m_orchList.rend(); ++it) {
        delete(*it);
    }
    delete m_select;

    events_deinit_publisher(g_events_handle);
}

bool OrchDaemon::init()
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";

    g_events_handle = events_init_publisher("sonic-events-swss");

    gCrmOrch = new CrmOrch(m_configDb, CFG_CRM_TABLE_NAME);

    TableConnector stateDbSwitchTable(m_stateDb, STATE_SWITCH_CAPABILITY_TABLE_NAME);
    TableConnector app_switch_table(m_applDb, APP_SWITCH_TABLE_NAME);
    TableConnector conf_asic_sensors(m_configDb, CFG_ASIC_SENSORS_TABLE_NAME);
    TableConnector conf_switch_hash(m_configDb, CFG_SWITCH_HASH_TABLE_NAME);
    TableConnector conf_suppress_asic_sdk_health_categories(m_configDb, CFG_SUPPRESS_ASIC_SDK_HEALTH_EVENT_NAME);

    vector<TableConnector> switch_tables = {
        conf_switch_hash,
        conf_asic_sensors,
        conf_suppress_asic_sdk_health_categories,
        app_switch_table
    };

    gSwitchOrch = new SwitchOrch(m_applDb, switch_tables, stateDbSwitchTable);

    const int portsorch_base_pri = 40;

    vector<table_name_with_pri_t> ports_tables = {
        { APP_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
        { APP_SEND_TO_INGRESS_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
        { APP_VLAN_TABLE_NAME,        portsorch_base_pri + 2 },
        { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri     },
        { APP_LAG_TABLE_NAME,         portsorch_base_pri + 4 },
        { APP_LAG_MEMBER_TABLE_NAME,  portsorch_base_pri     }
    };

    vector<table_name_with_pri_t> app_fdb_tables = {
        { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
        { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
        { APP_MCLAG_FDB_TABLE_NAME,  FdbOrch::fdborch_pri}
    };

    gPortsOrch = new PortsOrch(m_applDb, m_stateDb, ports_tables, m_chassisAppDb);
    TableConnector stateDbFdb(m_stateDb, STATE_FDB_TABLE_NAME);
    TableConnector stateMclagDbFdb(m_stateDb, STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
    gFdbOrch = new FdbOrch(m_applDb, app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);

    TableConnector stateDbBfdSessionTable(m_stateDb, STATE_BFD_SESSION_TABLE_NAME);

    BgpGlobalStateOrch* bgp_global_state_orch;
    bgp_global_state_orch = new BgpGlobalStateOrch(m_configDb, CFG_BGP_DEVICE_GLOBAL_TABLE_NAME);
    gDirectory.set(bgp_global_state_orch);

    gBfdOrch = new BfdOrch(m_applDb, APP_BFD_SESSION_TABLE_NAME, stateDbBfdSessionTable);
    gDirectory.set(gBfdOrch);
    static const  vector<string> route_pattern_tables = {
        CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
    };
    gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_configDb, route_pattern_tables);
    gDirectory.set(gFlowCounterRouteOrch);

    vector<string> vnet_tables = {
            APP_VNET_RT_TABLE_NAME,
            APP_VNET_RT_TUNNEL_TABLE_NAME
    };

    vector<string> cfg_vnet_tables = {
            CFG_VNET_RT_TABLE_NAME,
            CFG_VNET_RT_TUNNEL_TABLE_NAME
    };

    VNetOrch *vnet_orch;
    vnet_orch = new VNetOrch(m_applDb, APP_VNET_TABLE_NAME);

    gDirectory.set(vnet_orch);
    VNetCfgRouteOrch *cfg_vnet_rt_orch = new VNetCfgRouteOrch(m_configDb, m_applDb, cfg_vnet_tables);
    gDirectory.set(cfg_vnet_rt_orch);
    VNetRouteOrch *vnet_rt_orch = new VNetRouteOrch(m_applDb, vnet_tables, vnet_orch);
    gDirectory.set(vnet_rt_orch);
    VRFOrch *vrf_orch = new VRFOrch(m_applDb, APP_VRF_TABLE_NAME, m_stateDb, STATE_VRF_OBJECT_TABLE_NAME);
    gDirectory.set(vrf_orch);
    gMonitorOrch = new MonitorOrch(m_stateDb, STATE_VNET_MONITOR_TABLE_NAME);
    gDirectory.set(gMonitorOrch);

    const vector<string> chassis_frontend_tables = {
        CFG_PASS_THROUGH_ROUTE_TABLE_NAME,
    };
    ChassisOrch* chassis_frontend_orch = new ChassisOrch(m_configDb, m_applDb, chassis_frontend_tables, vnet_rt_orch);
    gDirectory.set(chassis_frontend_orch);

    gIntfsOrch = new IntfsOrch(m_applDb, APP_INTF_TABLE_NAME, vrf_orch, m_chassisAppDb);
    gNeighOrch = new NeighOrch(m_applDb, APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassisAppDb);

    const int fgnhgorch_pri = 15;

    vector<table_name_with_pri_t> fgnhg_tables = {
        { CFG_FG_NHG,                 fgnhgorch_pri },
        { CFG_FG_NHG_PREFIX,          fgnhgorch_pri },
        { CFG_FG_NHG_MEMBER,          fgnhgorch_pri }
    };

    gFgNhgOrch = new FgNhgOrch(m_configDb, m_applDb, m_stateDb, fgnhg_tables, gNeighOrch, gIntfsOrch, vrf_orch, m_zmqServer);
    gDirectory.set(gFgNhgOrch);

    vector<string> srv6_tables = {
        APP_SRV6_SID_LIST_TABLE_NAME,
        APP_SRV6_MY_SID_TABLE_NAME
    };
    gSrv6Orch = new Srv6Orch(m_applDb, srv6_tables, gSwitchOrch, vrf_orch, gNeighOrch);
    gDirectory.set(gSrv6Orch);

    const int routeorch_pri = 5;
    vector<table_name_with_pri_t> route_tables = {
        { APP_ROUTE_TABLE_NAME,        routeorch_pri },
        { APP_LABEL_ROUTE_TABLE_NAME,  routeorch_pri }
    };
    gRouteOrch = new RouteOrch(m_applDb, route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, vrf_orch, gFgNhgOrch, gSrv6Orch, m_zmqServer);
    gNhgOrch = new NhgOrch(m_applDb, APP_NEXTHOP_GROUP_TABLE_NAME);
    gCbfNhgOrch = new CbfNhgOrch(m_applDb, APP_CLASS_BASED_NEXT_HOP_GROUP_TABLE_NAME);

    gCoppOrch = new CoppOrch(m_applDb, APP_COPP_TABLE_NAME);

    vector<string> tunnel_tables = {
        APP_TUNNEL_DECAP_TABLE_NAME,
        APP_TUNNEL_DECAP_TERM_TABLE_NAME
    };
    gTunneldecapOrch = new TunnelDecapOrch(m_applDb, m_stateDb, m_configDb, tunnel_tables);
    gDirectory.set(gTunneldecapOrch);

    VxlanTunnelOrch *vxlan_tunnel_orch = new VxlanTunnelOrch(m_stateDb, m_applDb, APP_VXLAN_TUNNEL_TABLE_NAME);
    gDirectory.set(vxlan_tunnel_orch);
    VxlanTunnelMapOrch *vxlan_tunnel_map_orch = new VxlanTunnelMapOrch(m_applDb, APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
    gDirectory.set(vxlan_tunnel_map_orch);
    VxlanVrfMapOrch *vxlan_vrf_orch = new VxlanVrfMapOrch(m_applDb, APP_VXLAN_VRF_TABLE_NAME);
    gDirectory.set(vxlan_vrf_orch);


    EvpnNvoOrch* evpn_nvo_orch = new EvpnNvoOrch(m_applDb, APP_VXLAN_EVPN_NVO_TABLE_NAME);
    gDirectory.set(evpn_nvo_orch);

    NvgreTunnelOrch *nvgre_tunnel_orch = new NvgreTunnelOrch(m_configDb, CFG_NVGRE_TUNNEL_TABLE_NAME);
    gDirectory.set(nvgre_tunnel_orch);
    NvgreTunnelMapOrch *nvgre_tunnel_map_orch = new NvgreTunnelMapOrch(m_configDb, CFG_NVGRE_TUNNEL_MAP_TABLE_NAME);
    gDirectory.set(nvgre_tunnel_map_orch);

	vector<string> dash_vnet_tables = {
        APP_DASH_VNET_TABLE_NAME,
        APP_DASH_VNET_MAPPING_TABLE_NAME
    };
    DashVnetOrch *dash_vnet_orch = new DashVnetOrch(m_applDb, dash_vnet_tables, m_zmqServer);
    gDirectory.set(dash_vnet_orch);

    vector<string> dash_tables = {
        APP_DASH_APPLIANCE_TABLE_NAME,
        APP_DASH_ROUTING_TYPE_TABLE_NAME,
        APP_DASH_ENI_TABLE_NAME,
        APP_DASH_QOS_TABLE_NAME
    };

    DashOrch *dash_orch = new DashOrch(m_applDb, dash_tables, m_zmqServer);
    gDirectory.set(dash_orch);

    vector<string> dash_route_tables = {
        APP_DASH_ROUTE_TABLE_NAME,
        APP_DASH_ROUTE_RULE_TABLE_NAME
    };

    DashRouteOrch *dash_route_orch = new DashRouteOrch(m_applDb, dash_route_tables, dash_orch, m_zmqServer);
    gDirectory.set(dash_route_orch);

    vector<string> dash_acl_tables = {
        APP_DASH_PREFIX_TAG_TABLE_NAME,
        APP_DASH_ACL_IN_TABLE_NAME,
        APP_DASH_ACL_OUT_TABLE_NAME,
        APP_DASH_ACL_GROUP_TABLE_NAME,
        APP_DASH_ACL_RULE_TABLE_NAME
    };
    DashAclOrch *dash_acl_orch = new DashAclOrch(m_applDb, dash_acl_tables, dash_orch, m_zmqServer);
    gDirectory.set(dash_acl_orch);

    vector<string> qos_tables = {
        CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
        CFG_SCHEDULER_TABLE_NAME,
        CFG_DSCP_TO_TC_MAP_TABLE_NAME,
        CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
        CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
        CFG_QUEUE_TABLE_NAME,
        CFG_PORT_QOS_MAP_TABLE_NAME,
        CFG_WRED_PROFILE_TABLE_NAME,
        CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
        CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
        CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
        CFG_DSCP_TO_FC_MAP_TABLE_NAME,
        CFG_EXP_TO_FC_MAP_TABLE_NAME,
        CFG_TC_TO_DOT1P_MAP_TABLE_NAME,
        CFG_TC_TO_DSCP_MAP_TABLE_NAME
    };
    gQosOrch = new QosOrch(m_configDb, qos_tables);

    vector<string> buffer_tables = {
        APP_BUFFER_POOL_TABLE_NAME,
        APP_BUFFER_PROFILE_TABLE_NAME,
        APP_BUFFER_QUEUE_TABLE_NAME,
        APP_BUFFER_PG_TABLE_NAME,
        APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
        APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
    };
    gBufferOrch = new BufferOrch(m_applDb, m_configDb, m_stateDb, buffer_tables);

    vector<TableConnector> policer_tables = {
        TableConnector(m_configDb, CFG_POLICER_TABLE_NAME),
        TableConnector(m_configDb, CFG_PORT_STORM_CONTROL_TABLE_NAME)
    };

    TableConnector stateDbStorm(m_stateDb, "BUM_STORM_CAPABILITY");
    gPolicerOrch = new PolicerOrch(policer_tables, gPortsOrch);

    TableConnector stateDbMirrorSession(m_stateDb, STATE_MIRROR_SESSION_TABLE_NAME);
    TableConnector confDbMirrorSession(m_configDb, CFG_MIRROR_SESSION_TABLE_NAME);
    gMirrorOrch = new MirrorOrch(stateDbMirrorSession, confDbMirrorSession, gPortsOrch, gRouteOrch, gNeighOrch, gFdbOrch, gPolicerOrch);

    TableConnector confDbAclTable(m_configDb, CFG_ACL_TABLE_TABLE_NAME);
    TableConnector confDbAclTableType(m_configDb, CFG_ACL_TABLE_TYPE_TABLE_NAME);
    TableConnector confDbAclRuleTable(m_configDb, CFG_ACL_RULE_TABLE_NAME);
    TableConnector appDbAclTable(m_applDb, APP_ACL_TABLE_TABLE_NAME);
    TableConnector appDbAclTableType(m_applDb, APP_ACL_TABLE_TYPE_TABLE_NAME);
    TableConnector appDbAclRuleTable(m_applDb, APP_ACL_RULE_TABLE_NAME);

    vector<TableConnector> acl_table_connectors = {
        confDbAclTableType,
        confDbAclTable,
        confDbAclRuleTable,
        appDbAclTable,
        appDbAclRuleTable,
        appDbAclTableType,
    };

    vector<string> dtel_tables = {
        CFG_DTEL_TABLE_NAME,
        CFG_DTEL_REPORT_SESSION_TABLE_NAME,
        CFG_DTEL_INT_SESSION_TABLE_NAME,
        CFG_DTEL_QUEUE_REPORT_TABLE_NAME,
        CFG_DTEL_EVENT_TABLE_NAME
    };

    vector<string> wm_tables = {
        CFG_WATERMARK_TABLE_NAME,
        CFG_FLEX_COUNTER_TABLE_NAME
    };

    WatermarkOrch *wm_orch = new WatermarkOrch(m_configDb, wm_tables);

    vector<string> sflow_tables = {
            APP_SFLOW_TABLE_NAME,
            APP_SFLOW_SESSION_TABLE_NAME,
            APP_SFLOW_SAMPLE_RATE_TABLE_NAME
    };
    SflowOrch *sflow_orch = new SflowOrch(m_applDb,  sflow_tables);

    vector<string> debug_counter_tables = {
        CFG_DEBUG_COUNTER_TABLE_NAME,
        CFG_DEBUG_COUNTER_DROP_REASON_TABLE_NAME
    };

    gDebugCounterOrch = new DebugCounterOrch(m_configDb, debug_counter_tables, 1000);

    const int natorch_base_pri = 50;

    vector<table_name_with_pri_t> nat_tables = {
        { APP_NAT_DNAT_POOL_TABLE_NAME,  natorch_base_pri + 5 },
        { APP_NAT_TABLE_NAME,            natorch_base_pri + 4 },
        { APP_NAPT_TABLE_NAME,           natorch_base_pri + 3 },
        { APP_NAT_TWICE_TABLE_NAME,      natorch_base_pri + 2 },
        { APP_NAPT_TWICE_TABLE_NAME,     natorch_base_pri + 1 },
        { APP_NAT_GLOBAL_TABLE_NAME,     natorch_base_pri     }
    };

    gNatOrch = new NatOrch(m_applDb, m_stateDb, nat_tables, gRouteOrch, gNeighOrch);

    vector<string> mux_tables = {
        CFG_MUX_CABLE_TABLE_NAME,
        CFG_PEER_SWITCH_TABLE_NAME
    };
    MuxOrch *mux_orch = new MuxOrch(m_configDb, mux_tables, gTunneldecapOrch, gNeighOrch, gFdbOrch);
    gDirectory.set(mux_orch);

    MuxCableOrch *mux_cb_orch = new MuxCableOrch(m_applDb, m_stateDb, APP_MUX_CABLE_TABLE_NAME);
    gDirectory.set(mux_cb_orch);

    MuxStateOrch *mux_st_orch = new MuxStateOrch(m_stateDb, STATE_HW_MUX_CABLE_TABLE_NAME);
    gDirectory.set(mux_st_orch);

    vector<string> macsec_app_tables = {
        APP_MACSEC_PORT_TABLE_NAME,
        APP_MACSEC_EGRESS_SC_TABLE_NAME,
        APP_MACSEC_INGRESS_SC_TABLE_NAME,
        APP_MACSEC_EGRESS_SA_TABLE_NAME,
        APP_MACSEC_INGRESS_SA_TABLE_NAME,
    };

    gMacsecOrch = new MACsecOrch(m_applDb, m_stateDb, macsec_app_tables, gPortsOrch);

    gNhgMapOrch = new NhgMapOrch(m_applDb, APP_FC_TO_NHG_INDEX_MAP_TABLE_NAME);

    /*
     * The order of the orch list is important for state restore of warm start and
     * the queued processing in m_toSync map after gPortsOrch->allPortsReady() is set.
     *
     * For the multiple consumers in Orchs, tasks in a table which name is smaller in lexicographic order are processed first
     * when iterating ConsumerMap. This is ensured implicitly by the order of keys in ordered map.
     * For cases when Orch has to process tables in specific order, like PortsOrch during warm start, it has to override Orch::doTask()
     */
    m_orchList = { gSwitchOrch, gCrmOrch, gPortsOrch, gBufferOrch, gFlowCounterRouteOrch, gIntfsOrch, gNeighOrch, gNhgMapOrch, gNhgOrch, gCbfNhgOrch, gRouteOrch, gCoppOrch, gQosOrch, wm_orch, gPolicerOrch, gTunneldecapOrch, sflow_orch, gDebugCounterOrch, gMacsecOrch, bgp_global_state_orch, gBfdOrch, gSrv6Orch, mux_orch, mux_cb_orch, gMonitorOrch};

    bool initialize_dtel = false;
    if (platform == BFN_PLATFORM_SUBSTRING || platform == VS_PLATFORM_SUBSTRING)
    {
        sai_attr_capability_t capability;
        capability.create_implemented = true;

    /* Will uncomment this when saiobject.h support is added to SONiC */
    /*
    sai_status_t status;

        status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_DTEL, SAI_DTEL_ATTR_SWITCH_ID, &capability);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Could not query Dataplane telemetry capability %d", status);
            exit(EXIT_FAILURE);
        }
    */

        if (capability.create_implemented)
        {
            initialize_dtel = true;
        }
    }

    DTelOrch *dtel_orch = NULL;
    if (initialize_dtel)
    {
        dtel_orch = new DTelOrch(m_configDb, dtel_tables, gPortsOrch);
        m_orchList.push_back(dtel_orch);
    }

    gAclOrch = new AclOrch(acl_table_connectors, m_stateDb,
        gSwitchOrch, gPortsOrch, gMirrorOrch, gNeighOrch, gRouteOrch, dtel_orch);

    vector<string> mlag_tables = {
        { CFG_MCLAG_TABLE_NAME },
        { CFG_MCLAG_INTF_TABLE_NAME }
    };
    gMlagOrch = new MlagOrch(m_configDb, mlag_tables);

    TableConnector appDbIsoGrpTbl(m_applDb, APP_ISOLATION_GROUP_TABLE_NAME);
    vector<TableConnector> iso_grp_tbl_ctrs = {
        appDbIsoGrpTbl
    };

    gIsoGrpOrch = new IsoGrpOrch(iso_grp_tbl_ctrs);

    //
    // Policy Based Hashing (PBH) orchestrator
    //

    TableConnector cfgDbPbhTable(m_configDb, CFG_PBH_TABLE_TABLE_NAME);
    TableConnector cfgDbPbhRuleTable(m_configDb, CFG_PBH_RULE_TABLE_NAME);
    TableConnector cfgDbPbhHashTable(m_configDb, CFG_PBH_HASH_TABLE_NAME);
    TableConnector cfgDbPbhHashFieldTable(m_configDb, CFG_PBH_HASH_FIELD_TABLE_NAME);

    vector<TableConnector> pbhTableConnectorList = {
        cfgDbPbhTable,
        cfgDbPbhRuleTable,
        cfgDbPbhHashTable,
        cfgDbPbhHashFieldTable
    };

    gPbhOrch = new PbhOrch(pbhTableConnectorList, gAclOrch, gPortsOrch);

    m_orchList.push_back(gFdbOrch);
    m_orchList.push_back(gMirrorOrch);
    m_orchList.push_back(gAclOrch);
    m_orchList.push_back(gPbhOrch);
    m_orchList.push_back(chassis_frontend_orch);
    m_orchList.push_back(vrf_orch);
    m_orchList.push_back(vxlan_tunnel_orch);
    m_orchList.push_back(evpn_nvo_orch);
    m_orchList.push_back(vxlan_tunnel_map_orch);

    if (vxlan_tunnel_orch->isDipTunnelsSupported())
    {
        EvpnRemoteVnip2pOrch* evpn_remote_vni_orch = new EvpnRemoteVnip2pOrch(m_applDb, APP_VXLAN_REMOTE_VNI_TABLE_NAME);
        gDirectory.set(evpn_remote_vni_orch);
        m_orchList.push_back(evpn_remote_vni_orch);
    }
    else
    {
        EvpnRemoteVnip2mpOrch* evpn_remote_vni_orch = new EvpnRemoteVnip2mpOrch(m_applDb, APP_VXLAN_REMOTE_VNI_TABLE_NAME);
        gDirectory.set(evpn_remote_vni_orch);
        m_orchList.push_back(evpn_remote_vni_orch);
    }

    m_orchList.push_back(vxlan_vrf_orch);
    m_orchList.push_back(cfg_vnet_rt_orch);
    m_orchList.push_back(vnet_orch);
    m_orchList.push_back(vnet_rt_orch);
    m_orchList.push_back(gNatOrch);
    m_orchList.push_back(gMlagOrch);
    m_orchList.push_back(gIsoGrpOrch);
    m_orchList.push_back(gFgNhgOrch);
    m_orchList.push_back(mux_st_orch);
    m_orchList.push_back(nvgre_tunnel_orch);
    m_orchList.push_back(nvgre_tunnel_map_orch);
    m_orchList.push_back(dash_acl_orch);
    m_orchList.push_back(dash_vnet_orch);
    m_orchList.push_back(dash_route_orch);
    m_orchList.push_back(dash_orch);

    if (m_fabricEnabled)
    {
        // register APP_FABRIC_MONITOR_PORT_TABLE_NAME table
        const int fabric_portsorch_base_pri = 30;
        vector<table_name_with_pri_t> fabric_port_tables = {
           { APP_FABRIC_MONITOR_PORT_TABLE_NAME, fabric_portsorch_base_pri },
           { APP_FABRIC_MONITOR_DATA_TABLE_NAME, fabric_portsorch_base_pri }
        };
        gFabricPortsOrch = new FabricPortsOrch(m_applDb, fabric_port_tables, m_fabricPortStatEnabled, m_fabricQueueStatEnabled);
        m_orchList.push_back(gFabricPortsOrch);
    }

    vector<string> flex_counter_tables = {
        CFG_FLEX_COUNTER_TABLE_NAME
    };

    auto* flexCounterOrch = new FlexCounterOrch(m_configDb, flex_counter_tables);
    m_orchList.push_back(flexCounterOrch);

    gDirectory.set(flexCounterOrch);
    gDirectory.set(gPortsOrch);

    vector<string> pfc_wd_tables = {
        CFG_PFC_WD_TABLE_NAME
    };

    if ((platform == MLNX_PLATFORM_SUBSTRING)  || (platform == VS_PLATFORM_SUBSTRING))
    {

        static const vector<sai_port_stat_t> portStatIds =
        {
            SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION_US,
            SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION_US,
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
                    PFC_WD_POLL_MSECS));
    }
    else if ((platform == INVM_PLATFORM_SUBSTRING)
             || (platform == BFN_PLATFORM_SUBSTRING)
             || (platform == MRVL_PLATFORM_SUBSTRING)
             || (platform == NPS_PLATFORM_SUBSTRING))
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

        if ((platform == INVM_PLATFORM_SUBSTRING) || (platform == NPS_PLATFORM_SUBSTRING) || (platform == MRVL_PLATFORM_SUBSTRING))
        {
            m_orchList.push_back(new PfcWdSwOrch<PfcWdZeroBufferHandler, PfcWdLossyHandler>(
                        m_configDb,
                        pfc_wd_tables,
                        portStatIds,
                        queueStatIds,
                        queueAttrIds,
                        PFC_WD_POLL_MSECS));
        }
        else if (platform == BFN_PLATFORM_SUBSTRING)
        {
            m_orchList.push_back(new PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>(
                        m_configDb,
                        pfc_wd_tables,
                        portStatIds,
                        queueStatIds,
                        queueAttrIds,
                        PFC_WD_POLL_MSECS));
        }
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

        bool pfcDlrInit = gSwitchOrch->checkPfcDlrInitEnable();

        // Override pfcDlrInit if needed, and this change is only for PFC tests.
        if(getenv("PFC_DLR_INIT_ENABLE"))
        {
            string envPfcDlrInit = getenv("PFC_DLR_INIT_ENABLE");
            if(envPfcDlrInit == "1")
            {
                pfcDlrInit = true;
                SWSS_LOG_NOTICE("Override PfcDlrInitEnable to true");
            }
            else if(envPfcDlrInit == "0")
            {
                pfcDlrInit = false;
                SWSS_LOG_NOTICE("Override PfcDlrInitEnable to false");
            }
        }

        if(pfcDlrInit)
        {
            m_orchList.push_back(new PfcWdSwOrch<PfcWdDlrHandler, PfcWdDlrHandler>(
                        m_configDb,
                        pfc_wd_tables,
                        portStatIds,
                        queueStatIds,
                        queueAttrIds,
                        PFC_WD_POLL_MSECS));
        }
        else
        {
            m_orchList.push_back(new PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>(
                        m_configDb,
                        pfc_wd_tables,
                        portStatIds,
                        queueStatIds,
                        queueAttrIds,
                        PFC_WD_POLL_MSECS));
        }
    } else if (platform == CISCO_8000_PLATFORM_SUBSTRING)
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
            SAI_PORT_STAT_PFC_0_TX_PKTS,
            SAI_PORT_STAT_PFC_1_TX_PKTS,
            SAI_PORT_STAT_PFC_2_TX_PKTS,
            SAI_PORT_STAT_PFC_3_TX_PKTS,
            SAI_PORT_STAT_PFC_4_TX_PKTS,
            SAI_PORT_STAT_PFC_5_TX_PKTS,
            SAI_PORT_STAT_PFC_6_TX_PKTS,
            SAI_PORT_STAT_PFC_7_TX_PKTS,
        };

        static const vector<sai_queue_stat_t> queueStatIds =
        {
            SAI_QUEUE_STAT_PACKETS,
        };

        static const vector<sai_queue_attr_t> queueAttrIds =
        {
            SAI_QUEUE_ATTR_PAUSE_STATUS,
        };

        m_orchList.push_back(new PfcWdSwOrch<PfcWdSaiDlrInitHandler, PfcWdActionHandler>(
                    m_configDb,
                    pfc_wd_tables,
                    portStatIds,
                    queueStatIds,
                    queueAttrIds,
                    PFC_WD_POLL_MSECS));
    }

    m_orchList.push_back(&CounterCheckOrch::getInstance(m_configDb));

    vector<string> p4rt_tables = {APP_P4RT_TABLE_NAME};
    gP4Orch = new P4Orch(m_applDb, p4rt_tables, vrf_orch, gCoppOrch);
    m_orchList.push_back(gP4Orch);

    TableConnector confDbTwampTable(m_configDb, CFG_TWAMP_SESSION_TABLE_NAME);
    TableConnector stateDbTwampTable(m_stateDb, STATE_TWAMP_SESSION_TABLE_NAME);
    TwampOrch *twamp_orch = new TwampOrch(confDbTwampTable, stateDbTwampTable, gSwitchOrch, gPortsOrch, vrf_orch);
    m_orchList.push_back(twamp_orch);

    if (WarmStart::isWarmStart())
    {
        bool suc = warmRestoreAndSyncUp();
        if (!suc)
        {
            return false;
        }
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
        handleSaiFailure(true);
    }

    for (auto* orch: m_orchList)
    {
        orch->flushResponses();
    }
}

/* Release the file handle so the log can be rotated */
void OrchDaemon::logRotate() {
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    attr.id = SAI_REDIS_SWITCH_ATTR_PERFORM_LOG_ROTATE;
    attr.value.booldata = true;
    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to release the file handle on sairedis log %d", status);
    }
}


void OrchDaemon::start()
{
    SWSS_LOG_ENTER();

    Recorder::Instance().sairedis.setRotate(false);

    for (Orch *o : m_orchList)
    {
        m_select->addSelectables(o->getSelectables());
    }

    auto tstart = std::chrono::high_resolution_clock::now();

    while (true)
    {
        Selectable *s;
        int ret;

        ret = m_select->select(&s, SELECT_TIMEOUT);

        auto tend = std::chrono::high_resolution_clock::now();
        heartBeat(tend);

        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(tend - tstart);

        if (diff.count() >= SELECT_TIMEOUT)
        {
            tstart = std::chrono::high_resolution_clock::now();

            flush();
        }

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

        // check if logroate is requested
        if (Recorder::Instance().sairedis.isRotate())
        {
            SWSS_LOG_NOTICE("Performing %s log rotate", Recorder::Instance().sairedis.getName().c_str());
            Recorder::Instance().sairedis.setRotate(false);
            logRotate();
        }

        auto *c = (Executor *)s;
        c->execute();

        /* After each iteration, periodically check all m_toSync map to
         * execute all the remaining tasks that need to be retried. */

        /* TODO: Abstract Orch class to have a specific todo list */
        for (Orch *o : m_orchList)
            o->doTask();

        /*
         * Asked to check warm restart readiness.
         * Not doing this under Select::TIMEOUT condition because of
         * the existence of finer granularity ExecutableTimer with select
         */
        if (gSwitchOrch && gSwitchOrch->checkRestartReady())
        {
            bool ret = warmRestartCheck();
            if (ret)
            {
                // Orchagent is ready to perform warm restart, stop processing any new db data.
                // Should sleep here or continue handling timers and etc.??
                if (!gSwitchOrch->checkRestartNoFreeze())
                {
                    // Disable FDB aging
                    gSwitchOrch->setAgingFDB(0);

                    // Disable FDB learning on all bridge ports
                    if (gPortsOrch)
                    {
                        for (auto& pair: gPortsOrch->getAllPorts())
                        {
                            auto& port = pair.second;
                            gPortsOrch->setBridgePortLearningFDB(port, SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE);
                        }
                    }

                    // Flush sairedis's redis pipeline
                    flush();

                    SWSS_LOG_WARN("Orchagent is frozen for warm restart!");
                    freezeAndHeartBeat(UINT_MAX);
                }
            }
        }
    }
}

/*
 * Try to perform orchagent state restore and dynamic states sync up if
 * warm start request is detected.
 */
bool OrchDaemon::warmRestoreAndSyncUp()
{
    WarmStart::setWarmStartState("orchagent", WarmStart::INITIALIZED);

    for (Orch *o : m_orchList)
    {
        o->bake();
    }

    /*
     * Three iterations are needed.
     *
     * First iteration: switchorch, Port init/hostif create part of portorch, buffers configuration
     *
     * Second iteration: port speed/mtu/fec_mode/pfc_asym/admin_status config,
     * other orch(s) which wait for port to become ready.
     *
     * Third iteration: Drain remaining data that are out of order.
     */

    for (auto it = 0; it < 3; it++)
    {
        SWSS_LOG_DEBUG("The current doTask iteration is %d", it);

        for (Orch *o : m_orchList)
        {
            if (o == gMirrorOrch) {
                SWSS_LOG_DEBUG("Skipping mirror processing until the end");
                continue;
            }

            o->doTask();
        }
    }

    // MirrorOrch depends on everything else being settled before it can run,
    // and mirror ACL rules depend on MirrorOrch, so run these two at the end
    // after the rest of the data has been processed.
    gMirrorOrch->doTask();
    gAclOrch->doTask();

    /*
     * At this point, all the pre-existing data should have been processed properly, and
     * orchagent should be in exact same state of pre-shutdown.
     * Perform restore validation as needed.
     */
    bool suc = warmRestoreValidation();
    if (!suc)
    {
        SWSS_LOG_ERROR("Orchagent state restore failed");
        return false;
    }

    SWSS_LOG_NOTICE("Orchagent state restore done");

    syncd_apply_view();

    /* Start dynamic state sync up */
    gPortsOrch->refreshPortStatus();

    /*
     * Note. Arp sync up is handled in neighsyncd.
     * The "RECONCILED" state of orchagent doesn't mean the state related to neighbor is up to date.
     */
    WarmStart::setWarmStartState("orchagent", WarmStart::RECONCILED);
    return true;
}

/*
 * Get tasks to sync for consumers of each orch being managed by this orch daemon
 */
void OrchDaemon::getTaskToSync(vector<string> &ts)
{
    for (Orch *o : m_orchList)
    {
        o->dumpPendingTasks(ts);
    }
}


/* Perform basic validation after start restore for warm start */
bool OrchDaemon::warmRestoreValidation()
{
    /*
     * No pending task should exist for any of the consumer at this point.
     * All the prexisting data in appDB and configDb have been read and processed.
     */
    vector<string> ts;
    getTaskToSync(ts);
    if (ts.size() != 0)
    {
        // TODO: Update this section accordingly once pre-warmStart consistency validation is ready.
        SWSS_LOG_NOTICE("There are pending consumer tasks after restore: ");
        for(auto &s : ts)
        {
            SWSS_LOG_NOTICE("%s", s.c_str());
        }
    }
    WarmStart::setWarmStartState("orchagent", WarmStart::RESTORED);
    return ts.empty();
}

/*
 * Reply with "READY" notification if no pending tasks, and return true.
 * Ortherwise reply with "NOT_READY" notification and return false.
 * Further consideration is needed as to when orchagent is treated as warm restart ready.
 * For now, no pending task should exist in any orch agent.
 */
bool OrchDaemon::warmRestartCheck()
{
    std::vector<swss::FieldValueTuple> values;
    std::string op = "orchagent";
    std::string data = "READY";
    bool ret = true;

    vector<string> ts;
    getTaskToSync(ts);

    if (ts.size() != 0)
    {
        SWSS_LOG_NOTICE("WarmRestart check found pending tasks: ");
        for(auto &s : ts)
        {
            SWSS_LOG_NOTICE("    %s", s.c_str());
        }
        if (!gSwitchOrch->skipPendingTaskCheck())
        {
            data = "NOT_READY";
            ret = false;
        }
        else
        {
            SWSS_LOG_NOTICE("Orchagent objects dependency check skipped");
        }
    }

    SWSS_LOG_NOTICE("Restart check result: %s", data.c_str());
    gSwitchOrch->restartCheckReply(op,  data, values);
    return ret;
}

void OrchDaemon::addOrchList(Orch *o)
{
    m_orchList.push_back(o);
}

void OrchDaemon::heartBeat(std::chrono::time_point<std::chrono::high_resolution_clock> tcurrent)
{
    // output heart beat message to SYSLOG
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(tcurrent - m_lastHeartBeat);
    if (diff.count() >= HEART_BEAT_INTERVAL_MSECS)
    {
        m_lastHeartBeat = tcurrent;
        // output heart beat message to supervisord with 'PROCESS_COMMUNICATION_STDOUT' event: http://supervisord.org/events.html
        cout << "<!--XSUPERVISOR:BEGIN-->heartbeat<!--XSUPERVISOR:END-->" << endl;
    }
}

void OrchDaemon::freezeAndHeartBeat(unsigned int duration)
{
    while (duration > 0)
    {
        // Send heartbeat message to prevent Orchagent stuck alert.
        auto tend = std::chrono::high_resolution_clock::now();
        heartBeat(tend);

        duration--;
        sleep(1);
    }
}

FabricOrchDaemon::FabricOrchDaemon(DBConnector *applDb, DBConnector *configDb, DBConnector *stateDb, DBConnector *chassisAppDb, ZmqServer *zmqServer) :
    OrchDaemon(applDb, configDb, stateDb, chassisAppDb, zmqServer),
    m_applDb(applDb),
    m_configDb(configDb)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("FabricOrchDaemon starting...");
}

bool FabricOrchDaemon::init()
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("FabricOrchDaemon init");

    const int fabric_portsorch_base_pri = 30;
    vector<table_name_with_pri_t> fabric_port_tables = {
        { APP_FABRIC_MONITOR_PORT_TABLE_NAME, fabric_portsorch_base_pri },
        { APP_FABRIC_MONITOR_DATA_TABLE_NAME, fabric_portsorch_base_pri }
    };
    gFabricPortsOrch = new FabricPortsOrch(m_applDb, fabric_port_tables);
    addOrchList(gFabricPortsOrch);

    vector<string> flex_counter_tables = {
        CFG_FLEX_COUNTER_TABLE_NAME
    };
    addOrchList(new FlexCounterOrch(m_configDb, flex_counter_tables));

    return true;
}
