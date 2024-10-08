#pragma once

#include "orch.h"
#include "switchorch.h"
#include "crmorch.h"
#include "portsorch.h"
#include "routeorch.h"
#include "flowcounterrouteorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "fdborch.h"
#include "mirrororch.h"
#define private public
#include "dashorch.h"
#include "bufferorch.h"
#include "qosorch.h"
#define protected public
#include "pfcwdorch.h"
#undef protected
#undef private
#include "vrforch.h"
#include "vnetorch.h"
#include "vxlanorch.h"
#include "policerorch.h"
#include "fgnhgorch.h"
#include "flexcounterorch.h"
#include "tunneldecaporch.h"
#include "muxorch.h"
#include "nhgorch.h"
#include "copporch.h"
#include "twamporch.h"
#include "directory.h"

extern int gBatchSize;

extern MacAddress gMacAddress;
extern MacAddress gVxlanMacAddress;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gUnderlayIfId;

extern SwitchOrch *gSwitchOrch;
extern CrmOrch *gCrmOrch;
extern PortsOrch *gPortsOrch;
extern FgNhgOrch *gFgNhgOrch;
extern RouteOrch *gRouteOrch;
extern FlowCounterRouteOrch *gFlowCounterRouteOrch;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern FdbOrch *gFdbOrch;
extern MirrorOrch *gMirrorOrch;
extern BufferOrch *gBufferOrch;
extern QosOrch *gQosOrch;
template <typename DropHandler, typename ForwardHandler> PfcWdSwOrch<DropHandler, ForwardHandler> *gPfcwdOrch;
extern VRFOrch *gVrfOrch;
extern NhgOrch *gNhgOrch;
extern Srv6Orch  *gSrv6Orch;
extern BfdOrch *gBfdOrch;
extern AclOrch *gAclOrch;
extern PolicerOrch *gPolicerOrch;
extern TunnelDecapOrch *gTunneldecapOrch;
extern Directory<Orch*> gDirectory;

extern sai_acl_api_t *sai_acl_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_hash_api_t *sai_hash_api;
extern sai_virtual_router_api_t *sai_virtual_router_api;
extern sai_port_api_t *sai_port_api;
extern sai_lag_api_t *sai_lag_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_bridge_api_t *sai_bridge_api;
extern sai_router_interface_api_t *sai_router_intfs_api;
extern sai_route_api_t *sai_route_api;
extern sai_neighbor_api_t *sai_neighbor_api;
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_next_hop_api_t *sai_next_hop_api;
extern sai_next_hop_group_api_t *sai_next_hop_group_api;
extern sai_hostif_api_t *sai_hostif_api;
extern sai_policer_api_t *sai_policer_api;
extern sai_buffer_api_t *sai_buffer_api;
extern sai_qos_map_api_t *sai_qos_map_api;
extern sai_scheduler_api_t *sai_scheduler_api;
extern sai_scheduler_group_api_t *sai_scheduler_group_api;
extern sai_wred_api_t *sai_wred_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_udf_api_t* sai_udf_api;
extern sai_mpls_api_t* sai_mpls_api;
extern sai_counter_api_t* sai_counter_api;
extern sai_samplepacket_api_t *sai_samplepacket_api;
extern sai_fdb_api_t* sai_fdb_api;
extern sai_twamp_api_t* sai_twamp_api;
extern sai_tam_api_t* sai_tam_api;
extern sai_dash_vip_api_t* sai_dash_vip_api;
extern sai_dash_direction_lookup_api_t* sai_dash_direction_lookup_api;
extern sai_dash_eni_api_t* sai_dash_eni_api;
