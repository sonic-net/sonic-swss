#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <inttypes.h>
#include <math.h>
#include <chrono>
#include <time.h>

#include "sai.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "orch.h"
#include "request_parser.h"
#include "muxorch.h"
#include "directory.h"
#include "swssnet.h"
#include "crmorch.h"
#include "neighorch.h"
#include "portsorch.h"
#include "aclorch.h"
#include "routeorch.h"
#include "fdborch.h"
#include "qosorch.h"

/* Global variables */
extern Directory<Orch*> gDirectory;
extern CrmOrch *gCrmOrch;
extern NeighOrch *gNeighOrch;
extern RouteOrch *gRouteOrch;
extern AclOrch *gAclOrch;
extern PortsOrch *gPortsOrch;
extern FdbOrch *gFdbOrch;
extern QosOrch *gQosOrch;

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_object_id_t gSwitchId;
extern sai_route_api_t* sai_route_api;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_router_interface_api_t* sai_router_intfs_api;

/* Constants */
#define MUX_ACL_TABLE_NAME INGRESS_TABLE_DROP
#define MUX_ACL_RULE_NAME "mux_acl_rule"
#define MUX_HW_STATE_UNKNOWN "unknown"
#define MUX_HW_STATE_ERROR "error"

const map<std::pair<MuxState, MuxState>, MuxStateChange> muxStateTransition =
{
    { { MuxState::MUX_STATE_INIT, MuxState::MUX_STATE_ACTIVE}, MuxStateChange::MUX_STATE_INIT_ACTIVE
    },

    { { MuxState::MUX_STATE_INIT, MuxState::MUX_STATE_STANDBY}, MuxStateChange::MUX_STATE_INIT_STANDBY
    },

    { { MuxState::MUX_STATE_ACTIVE, MuxState::MUX_STATE_STANDBY}, MuxStateChange::MUX_STATE_ACTIVE_STANDBY
    },

    { { MuxState::MUX_STATE_STANDBY, MuxState::MUX_STATE_ACTIVE}, MuxStateChange::MUX_STATE_STANDBY_ACTIVE
    },
};

const map <MuxState, string> muxStateValToString =
{
    { MuxState::MUX_STATE_ACTIVE, "active" },
    { MuxState::MUX_STATE_STANDBY, "standby" },
    { MuxState::MUX_STATE_INIT, "init" },
    { MuxState::MUX_STATE_FAILED, "failed" },
    { MuxState::MUX_STATE_PENDING, "pending" },
};

const map <string, MuxState> muxStateStringToVal =
{
    { "active", MuxState::MUX_STATE_ACTIVE },
    { "standby", MuxState::MUX_STATE_STANDBY },
    { "unknown", MuxState::MUX_STATE_STANDBY },
    { "init", MuxState::MUX_STATE_INIT },
    { "failed", MuxState::MUX_STATE_FAILED },
    { "pending", MuxState::MUX_STATE_PENDING },
};

static inline MuxStateChange mux_state_change (MuxState prev, MuxState curr)
{
    auto key = std::make_pair(prev, curr);
    if (muxStateTransition.find(key) != muxStateTransition.end())
    {
        return muxStateTransition.at(key);
    }

    return MuxStateChange::MUX_STATE_UNKNOWN_STATE;
}

static sai_status_t create_route(IpPrefix &pfx, sai_object_id_t nh)
{
    sai_route_entry_t route_entry;
    route_entry.switch_id = gSwitchId;
    route_entry.vr_id = gVirtualRouterId;
    copy(route_entry.destination, pfx);
    subnet(route_entry.destination, route_entry.destination);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = nh;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route_entry(&route_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create tunnel route %s,nh %" PRIx64 " rv:%d",
                pfx.getIp().to_string().c_str(), nh, status);
        return status;
    }

    if (route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }

    SWSS_LOG_NOTICE("Created tunnel route to %s ", pfx.to_string().c_str());
    return status;
}

static sai_status_t remove_route(IpPrefix &pfx)
{
    sai_route_entry_t route_entry;
    route_entry.switch_id = gSwitchId;
    route_entry.vr_id = gVirtualRouterId;
    copy(route_entry.destination, pfx);
    subnet(route_entry.destination, route_entry.destination);

    sai_status_t status = sai_route_api->remove_route_entry(&route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove tunnel route %s, rv:%d",
                        pfx.getIp().to_string().c_str(), status);
        return status;
    }

    if (route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }

    SWSS_LOG_NOTICE("Removed tunnel route to %s ", pfx.to_string().c_str());
    return status;
}

static sai_object_id_t create_tunnel(
    const IpAddress* p_dst_ip,
    const IpAddress* p_src_ip,
    sai_object_id_t tc_to_dscp_map_id,
    sai_object_id_t tc_to_queue_map_id,
    string dscp_mode_name)
{
    sai_status_t status;

    sai_attribute_t attr;
    sai_object_id_t overlay_if;
    vector<sai_attribute_t> tunnel_attrs;
    vector<sai_attribute_t> overlay_intf_attrs;

    sai_attribute_t overlay_intf_attr;
    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    overlay_intf_attr.value.oid = gVirtualRouterId;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    overlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    status = sai_router_intfs_api->create_router_interface(&overlay_if, gSwitchId, (uint32_t)overlay_intf_attrs.size(), overlay_intf_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create overlay interface");
    }

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    attr.value.oid = overlay_if;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = gUnderlayIfId;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2P;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_ENCAP_TTL_MODE;
    attr.value.s32 = SAI_TUNNEL_TTL_MODE_PIPE_MODEL;
    tunnel_attrs.push_back(attr);

    if (dscp_mode_name == "uniform" || dscp_mode_name == "pipe")
    {
        sai_tunnel_dscp_mode_t dscp_mode;
        if (dscp_mode_name == "uniform")
        {
            dscp_mode = SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL;
        }
        else
        {
            dscp_mode = SAI_TUNNEL_DSCP_MODE_PIPE_MODEL;
        }
        attr.id = SAI_TUNNEL_ATTR_ENCAP_DSCP_MODE;
        attr.value.s32 = dscp_mode;
        tunnel_attrs.push_back(attr);
    }

    attr.id = SAI_TUNNEL_ATTR_LOOPBACK_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_DROP;
    tunnel_attrs.push_back(attr);

    if (p_src_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
        copy(attr.value.ipaddr, p_src_ip->to_string());
        tunnel_attrs.push_back(attr);
    }

    if (p_dst_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
        copy(attr.value.ipaddr, p_dst_ip->to_string());
        tunnel_attrs.push_back(attr);
    }

    // DSCP rewriting
    if (tc_to_dscp_map_id != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_QOS_TC_AND_COLOR_TO_DSCP_MAP;
        attr.value.oid = tc_to_dscp_map_id;
        tunnel_attrs.push_back(attr);
    }

    // TC remapping
    if (tc_to_queue_map_id != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_QOS_TC_TO_QUEUE_MAP;
        attr.value.oid = tc_to_queue_map_id;
        tunnel_attrs.push_back(attr);
    }

    sai_object_id_t tunnel_id;
    status = sai_tunnel_api->create_tunnel(&tunnel_id, gSwitchId, (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel object");
    }

    return tunnel_id;
}

static sai_object_id_t create_nh_tunnel(sai_object_id_t tunnel_id, IpAddress& ipAddr)
{
    std::vector<sai_attribute_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
    next_hop_attrs.push_back(next_hop_attr);

    sai_ip_address_t host_ip;
    swss::copy(host_ip, ipAddr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    next_hop_attr.value.ipaddr = host_ip;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    next_hop_attr.value.oid = tunnel_id;
    next_hop_attrs.push_back(next_hop_attr);

    sai_object_id_t next_hop_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_next_hop_api->create_next_hop(&next_hop_id, gSwitchId,
                                            static_cast<uint32_t>(next_hop_attrs.size()),
                                            next_hop_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Tunnel NH create failed for ip %s", ipAddr.to_string().c_str());
    }
    else
    {
        SWSS_LOG_NOTICE("Tunnel NH created for ip %s", ipAddr.to_string().c_str());

        if (ipAddr.isV4())
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
        }
    }

    return next_hop_id;
}

static bool remove_nh_tunnel(sai_object_id_t nh_id, IpAddress& ipAddr)
{
    sai_status_t status = sai_next_hop_api->remove_next_hop(nh_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate next hop %s rv:%d",
                            ipAddr.to_string().c_str(), status);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove next hop %s  rv:%d",
                            ipAddr.to_string().c_str(), status);
            return false;
        }
    }
    else
    {
        SWSS_LOG_NOTICE("Tunnel NH removed for ip %s",ipAddr.to_string().c_str());

        if (ipAddr.isV4())
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
        }
        else
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
        }
    }

    return true;
}

MuxCable::MuxCable(string name, IpPrefix& srv_ip4, IpPrefix& srv_ip6, IpAddress peer_ip, std::set<IpAddress> skip_neighbors)
         :mux_name_(name), srv_ip4_(srv_ip4), srv_ip6_(srv_ip6), peer_ip4_(peer_ip), skip_neighbors_(skip_neighbors)
{
    mux_orch_ = gDirectory.get<MuxOrch*>();
    mux_cb_orch_ = gDirectory.get<MuxCableOrch*>();
    mux_state_orch_ = gDirectory.get<MuxStateOrch*>();

    nbr_handler_ = std::make_unique<MuxNbrHandler> (MuxNbrHandler());

    state_machine_handlers_.insert(handler_pair(MUX_STATE_INIT_ACTIVE, &MuxCable::stateInitActive));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_STANDBY_ACTIVE, &MuxCable::stateActive));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_INIT_STANDBY, &MuxCable::stateStandby));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_ACTIVE_STANDBY, &MuxCable::stateStandby));

    /* Set initial state to "standby" */
    stateStandby();
}

bool MuxCable::stateInitActive()
{
    SWSS_LOG_INFO("Set state to Active from %s", muxStateValToString.at(state_).c_str());

    if (!nbrHandler(true, false))
    {
        return false;
    }

    return true;
}

bool MuxCable::stateActive()
{
    SWSS_LOG_INFO("Set state to Active for %s", mux_name_.c_str());

    Port port;
    if (!gPortsOrch->getPort(mux_name_, port))
    {
        SWSS_LOG_NOTICE("Port %s not found in port table", mux_name_.c_str());
        return false;
    }

    if (!aclHandler(port.m_port_id, mux_name_, false))
    {
        SWSS_LOG_INFO("Remove ACL drop rule failed for %s", mux_name_.c_str());
        return false;
    }

    if (!nbrHandler(true))
    {
        return false;
    }

    return true;
}

bool MuxCable::stateStandby()
{
    SWSS_LOG_INFO("Set state to Standby for %s", mux_name_.c_str());

    Port port;
    if (!gPortsOrch->getPort(mux_name_, port))
    {
        SWSS_LOG_NOTICE("Port %s not found in port table", mux_name_.c_str());
        return false;
    }

    if (!nbrHandler(false))
    {
        return false;
    }

    if (!aclHandler(port.m_port_id, mux_name_))
    {
        SWSS_LOG_INFO("Add ACL drop rule failed for %s", mux_name_.c_str());
        return false;
    }

    return true;
}

void MuxCable::setState(string new_state)
{
    SWSS_LOG_NOTICE("[%s] Set MUX state from %s to %s", mux_name_.c_str(),
                     muxStateValToString.at(state_).c_str(), new_state.c_str());

    MuxState ns = muxStateStringToVal.at(new_state);

    /* Update new_state to handle unknown state */
    new_state = muxStateValToString.at(ns);

    auto it = muxStateTransition.find(make_pair(state_, ns));

    if (it ==  muxStateTransition.end())
    {
        // Update HW Mux cable state anyways
        mux_cb_orch_->updateMuxState(mux_name_, new_state);
        SWSS_LOG_ERROR("State transition from %s to %s is not-handled ",
                        muxStateValToString.at(state_).c_str(), new_state.c_str());
        return;
    }

    mux_cb_orch_->updateMuxMetricState(mux_name_, new_state, true);

    MuxState state = state_;
    state_ = ns;

    st_chg_in_progress_ = true;

    if (!(this->*(state_machine_handlers_[it->second]))())
    {
        //Reset back to original state
        state_ = state;
        st_chg_in_progress_ = false;
        st_chg_failed_ = true;
        throw std::runtime_error("Failed to handle state transition");
    }

    mux_cb_orch_->updateMuxMetricState(mux_name_, new_state, false);

    st_chg_in_progress_ = false;
    st_chg_failed_ = false;
    SWSS_LOG_INFO("Changed state to %s", new_state.c_str());

    mux_cb_orch_->updateMuxState(mux_name_, new_state);
    return;
}

string MuxCable::getState()
{
    SWSS_LOG_INFO("Get state request for %s, state %s",
                   mux_name_.c_str(), muxStateValToString.at(state_).c_str());

    return (muxStateValToString.at(state_));
}

bool MuxCable::aclHandler(sai_object_id_t port, string alias, bool add)
{
    if (add)
    {
        acl_handler_ = make_shared<MuxAclHandler>(port, alias);
    }
    else
    {
        acl_handler_.reset();
    }

    return true;
}

bool MuxCable::isIpInSubnet(IpAddress ip)
{
    if (ip.isV4())
    {
        return (srv_ip4_.isAddressInSubnet(ip));
    }
    else
    {
        return (srv_ip6_.isAddressInSubnet(ip));
    }
}

bool MuxCable::nbrHandler(bool enable, bool update_rt)
{
    if (enable)
    {
        return nbr_handler_->enable(update_rt);
    }
    else
    {
        sai_object_id_t tnh = mux_orch_->createNextHopTunnel(MUX_TUNNEL, peer_ip4_);
        if (tnh == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_INFO("Null NH object id, retry for %s", peer_ip4_.to_string().c_str());
            return false;
        }

        return nbr_handler_->disable(tnh);
    }
}

void MuxCable::updateNeighbor(NextHopKey nh, bool add)
{
    sai_object_id_t tnh = mux_orch_->getNextHopTunnelId(MUX_TUNNEL, peer_ip4_);
    if (add && skip_neighbors_.find(nh.ip_address) != skip_neighbors_.end())
    {
        SWSS_LOG_INFO("Skip update neighbor %s on %s", nh.ip_address.to_string().c_str(), nh.alias.c_str());
        return;
    }
    nbr_handler_->update(nh, tnh, add, state_);
    if (add)
    {
        mux_orch_->addNexthop(nh, mux_name_);
    }
    else if (mux_name_ == mux_orch_->getNexthopMuxName(nh))
    {
        mux_orch_->removeNexthop(nh);
    }
}

void MuxNbrHandler::update(NextHopKey nh, sai_object_id_t tunnelId, bool add, MuxState state)
{
    SWSS_LOG_INFO("Neigh %s on %s, add %d, state %d",
                   nh.ip_address.to_string().c_str(), nh.alias.c_str(), add, state);

    MuxCableOrch* mux_cb_orch = gDirectory.get<MuxCableOrch*>();
    IpPrefix pfx = nh.ip_address.to_string();

    if (add)
    {
        if (!nh.alias.empty() && nh.alias != alias_)
        {
            alias_ = nh.alias;
        }

        if (neighbors_.find(nh.ip_address) != neighbors_.end())
        {
            return;
        }

        switch (state)
        {
        case MuxState::MUX_STATE_INIT:
            neighbors_[nh.ip_address] = SAI_NULL_OBJECT_ID;
            break;
        case MuxState::MUX_STATE_ACTIVE:
            neighbors_[nh.ip_address] = gNeighOrch->getLocalNextHopId(nh);
            gNeighOrch->enableNeighbor(nh);
            break;
        case MuxState::MUX_STATE_STANDBY:
            neighbors_[nh.ip_address] = tunnelId;
            gNeighOrch->disableNeighbor(nh);
            mux_cb_orch->addTunnelRoute(nh);
            create_route(pfx, tunnelId);
            break;
        default:
            SWSS_LOG_NOTICE("State '%s' not handled for nbr %s update",
                             muxStateValToString.at(state).c_str(), nh.ip_address.to_string().c_str());
            break;
        }
    }
    else
    {
        /* if current state is standby, remove the tunnel route */
        if (state == MuxState::MUX_STATE_STANDBY)
        {
            remove_route(pfx);
            mux_cb_orch->removeTunnelRoute(nh);
        }
        neighbors_.erase(nh.ip_address);
    }
}

bool MuxNbrHandler::enable(bool update_rt)
{
    NeighborEntry neigh;
    MuxCableOrch* mux_cb_orch = gDirectory.get<MuxCableOrch*>();

    auto it = neighbors_.begin();
    while (it != neighbors_.end())
    {
        SWSS_LOG_INFO("Enabling neigh %s on %s", it->first.to_string().c_str(), alias_.c_str());

        neigh = NeighborEntry(it->first, alias_);
        if (!gNeighOrch->enableNeighbor(neigh))
        {
            SWSS_LOG_INFO("Enabling neigh failed for %s", neigh.ip_address.to_string().c_str());
            return false;
        }

        /* Update NH to point to learned neighbor */
        it->second = gNeighOrch->getLocalNextHopId(neigh);

        /* Reprogram route */
        NextHopKey nh_key = NextHopKey(it->first, alias_);
        uint32_t num_routes = 0;
        if (!gRouteOrch->updateNextHopRoutes(nh_key, num_routes))
        {
            SWSS_LOG_INFO("Update route failed for NH %s", nh_key.ip_address.to_string().c_str());
            return false;
        }

        /* Increment ref count for new NHs */
        gNeighOrch->increaseNextHopRefCount(nh_key, num_routes);

        /*
         * Invalidate current nexthop group and update with new NH
         * Ref count update is not required for tunnel NH IDs (nh_removed)
         */
        uint32_t nh_removed, nh_added;
        if (!gRouteOrch->invalidnexthopinNextHopGroup(nh_key, nh_removed))
        {
            SWSS_LOG_ERROR("Removing existing NH failed for %s", nh_key.ip_address.to_string().c_str());
            return false;
        }

        if (!gRouteOrch->validnexthopinNextHopGroup(nh_key, nh_added))
        {
            SWSS_LOG_ERROR("Adding NH failed for %s", nh_key.ip_address.to_string().c_str());
            return false;
        }

        /* Increment ref count for ECMP NH members */
        gNeighOrch->increaseNextHopRefCount(nh_key, nh_added);

        IpPrefix pfx = it->first.to_string();
        if (update_rt)
        {
            if (remove_route(pfx) != SAI_STATUS_SUCCESS)
            {
                return false;
            }
            mux_cb_orch->removeTunnelRoute(nh_key);
        }

        it++;
    }

    return true;
}

bool MuxNbrHandler::disable(sai_object_id_t tnh)
{
    NeighborEntry neigh;
    MuxCableOrch* mux_cb_orch = gDirectory.get<MuxCableOrch*>();

    auto it = neighbors_.begin();
    while (it != neighbors_.end())
    {
        SWSS_LOG_INFO("Disabling neigh %s on %s", it->first.to_string().c_str(), alias_.c_str());

        /* Update NH to point to Tunnel nexhtop */
        it->second = tnh;

        /* Reprogram route */
        NextHopKey nh_key = NextHopKey(it->first, alias_);
        uint32_t num_routes = 0;
        if (!gRouteOrch->updateNextHopRoutes(nh_key, num_routes))
        {
            SWSS_LOG_INFO("Update route failed for NH %s", nh_key.ip_address.to_string().c_str());
            return false;
        }

        /* Decrement ref count for old NHs */
        gNeighOrch->decreaseNextHopRefCount(nh_key, num_routes);

        /* Invalidate current nexthop group and update with new NH */
        uint32_t nh_removed, nh_added;
        if (!gRouteOrch->invalidnexthopinNextHopGroup(nh_key, nh_removed))
        {
            SWSS_LOG_ERROR("Removing existing NH failed for %s", nh_key.ip_address.to_string().c_str());
            return false;
        }

        /* Decrement ref count for ECMP NH members */
        gNeighOrch->decreaseNextHopRefCount(nh_key, nh_removed);

        if (!gRouteOrch->validnexthopinNextHopGroup(nh_key, nh_added))
        {
            SWSS_LOG_ERROR("Adding NH failed for %s", nh_key.ip_address.to_string().c_str());
            return false;
        }

        neigh = NeighborEntry(it->first, alias_);
        if (!gNeighOrch->disableNeighbor(neigh))
        {
            SWSS_LOG_INFO("Disabling neigh failed for %s", neigh.ip_address.to_string().c_str());
            return false;
        }

        mux_cb_orch->addTunnelRoute(nh_key);

        IpPrefix pfx = it->first.to_string();
        if (create_route(pfx, it->second) != SAI_STATUS_SUCCESS)
        {
            return false;
        }

        it++;
    }

    return true;
}

sai_object_id_t MuxNbrHandler::getNextHopId(const NextHopKey nhKey)
{
    auto it = neighbors_.find(nhKey.ip_address);
    if (it != neighbors_.end())
    {
        return it->second;
    }

    return SAI_NULL_OBJECT_ID;
}

std::map<std::string, AclTable> MuxAclHandler::acl_table_;

MuxAclHandler::MuxAclHandler(sai_object_id_t port, string alias)
{
    SWSS_LOG_ENTER();

    // There is one handler instance per MUX port
    string table_name = MUX_ACL_TABLE_NAME;
    string rule_name = MUX_ACL_RULE_NAME;

    port_ = port;
    alias_ = alias;

    auto found = acl_table_.find(table_name);
    if (found == acl_table_.end())
    {
        SWSS_LOG_NOTICE("First time create for port %" PRIx64 "", port);

        // First time handling of Mux Table, create ACL table, and bind
        createMuxAclTable(port, table_name);
        shared_ptr<AclRulePacket> newRule =
                make_shared<AclRulePacket>(gAclOrch, rule_name, table_name);
        createMuxAclRule(newRule, table_name);
    }
    else
    {
        SWSS_LOG_NOTICE("Binding port %" PRIx64 "", port);

        AclRule* rule = gAclOrch->getAclRule(table_name, rule_name);
        if (rule == nullptr)
        {
            shared_ptr<AclRulePacket> newRule =
                    make_shared<AclRulePacket>(gAclOrch, rule_name, table_name);
            createMuxAclRule(newRule, table_name);
        }
        else
        {
            gAclOrch->updateAclRule(table_name, rule_name, MATCH_IN_PORTS, &port, RULE_OPER_ADD);
        }
    }
}

MuxAclHandler::~MuxAclHandler(void)
{
    SWSS_LOG_ENTER();
    string table_name = MUX_ACL_TABLE_NAME;
    string rule_name = MUX_ACL_RULE_NAME;

    SWSS_LOG_NOTICE("Un-Binding port %" PRIx64 "", port_);

    AclRule* rule = gAclOrch->getAclRule(table_name, rule_name);
    if (rule == nullptr)
    {
        SWSS_LOG_THROW("ACL Rule does not exist for port %s, rule %s", alias_.c_str(), rule_name.c_str());
    }

    vector<sai_object_id_t> port_set = rule->getInPorts();
    if ((port_set.size() == 1) && (port_set[0] == port_))
    {
        gAclOrch->removeAclRule(table_name, rule_name);
    }
    else
    {
        gAclOrch->updateAclRule(table_name, rule_name, MATCH_IN_PORTS, &port_, RULE_OPER_DELETE);
    }
}

void MuxAclHandler::createMuxAclTable(sai_object_id_t port, string strTable)
{
    SWSS_LOG_ENTER();

    auto inserted = acl_table_.emplace(piecewise_construct,
                                       std::forward_as_tuple(strTable),
                                       std::forward_as_tuple(gAclOrch, strTable));

    assert(inserted.second);

    AclTable& acl_table = inserted.first->second;

    sai_object_id_t table_oid = gAclOrch->getTableById(strTable);
    if (table_oid != SAI_NULL_OBJECT_ID)
    {
        // DROP ACL table is already created
        SWSS_LOG_NOTICE("ACL table %s exists, reuse the same", strTable.c_str());
        acl_table = *(gAclOrch->getTableByOid(table_oid));
        return;
    }

    auto dropType = gAclOrch->getAclTableType(TABLE_TYPE_DROP);
    assert(dropType);
    acl_table.validateAddType(*dropType);
    acl_table.stage = ACL_STAGE_INGRESS;
    gAclOrch->addAclTable(acl_table);
    bindAllPorts(acl_table);
}

void MuxAclHandler::createMuxAclRule(shared_ptr<AclRulePacket> rule, string strTable)
{
    SWSS_LOG_ENTER();

    string attr_name, attr_value;

    attr_name = RULE_PRIORITY;
    attr_value = "999";
    rule->validateAddPriority(attr_name, attr_value);

    // Add MATCH_IN_PORTS as match criteria for ingress table
    attr_name = MATCH_IN_PORTS;
    attr_value = alias_;
    rule->validateAddMatch(attr_name, attr_value);

    attr_name = ACTION_PACKET_ACTION;
    attr_value = PACKET_ACTION_DROP;
    rule->validateAddAction(attr_name, attr_value);

    gAclOrch->addAclRule(rule, strTable);
}

void MuxAclHandler::bindAllPorts(AclTable &acl_table)
{
    SWSS_LOG_ENTER();

    auto allPorts = gPortsOrch->getAllPorts();
    for (auto &it: allPorts)
    {
        Port port = it.second;
        if (port.m_type == Port::PHY)
        {
            SWSS_LOG_INFO("Binding port %" PRIx64 " to ACL table %s", port.m_port_id, acl_table.id.c_str());

            acl_table.link(port.m_port_id);
            acl_table.bind(port.m_port_id);
        }
    }
}

sai_object_id_t MuxOrch::createNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr)
{
    auto it = mux_tunnel_nh_.find(ipAddr);
    if (it != mux_tunnel_nh_.end())
    {
        return it->second.nh_id;
    }

    sai_object_id_t nh = create_nh_tunnel(mux_tunnel_id_, ipAddr);

    if (SAI_NULL_OBJECT_ID != nh)
    {
        mux_tunnel_nh_[ipAddr] = { nh, 1 };
    }

    return nh;
}

bool MuxOrch::removeNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr)
{
    auto it = mux_tunnel_nh_.find(ipAddr);
    if (it == mux_tunnel_nh_.end())
    {
        SWSS_LOG_NOTICE("NH doesn't exist %s, ip %s", tunnelKey.c_str(), ipAddr.to_string().c_str());
        return true;
    }

    auto ref_cnt = --it->second.ref_count;

    if (it->second.ref_count == 0)
    {
        if (!remove_nh_tunnel(it->second.nh_id, ipAddr))
        {
            SWSS_LOG_INFO("NH tunnel remove failed %s, ip %s",
                           tunnelKey.c_str(), ipAddr.to_string().c_str());
        }
        mux_tunnel_nh_.erase(ipAddr);
    }

    SWSS_LOG_INFO("NH tunnel removed  %s, ip %s or decremented to ref count %d",
                   tunnelKey.c_str(), ipAddr.to_string().c_str(), ref_cnt);
    return true;
}

sai_object_id_t MuxOrch::getNextHopTunnelId(std::string tunnelKey, IpAddress& ipAddr)
{
    auto it = mux_tunnel_nh_.find(ipAddr);
    if (it == mux_tunnel_nh_.end())
    {
        SWSS_LOG_INFO("NH doesn't exist %s, ip %s", tunnelKey.c_str(), ipAddr.to_string().c_str());
        return SAI_NULL_OBJECT_ID;
    }

    return it->second.nh_id;
}

MuxCable* MuxOrch::findMuxCableInSubnet(IpAddress ip)
{
    for (auto it = mux_cable_tb_.begin(); it != mux_cable_tb_.end(); it++)
    {
       MuxCable* ptr = it->second.get();
       if (ptr->isIpInSubnet(ip))
       {
           return ptr;
       }
    }

    return nullptr;
}

bool MuxOrch::isNeighborActive(const IpAddress& nbr, const MacAddress& mac, string& alias)
{
    if (mux_cable_tb_.empty())
    {
        return true;
    }

    MuxCable* ptr = findMuxCableInSubnet(nbr);

    if (ptr)
    {
        return ptr->isActive();
    }

    string port;
    if (!getMuxPort(mac, alias, port))
    {
        SWSS_LOG_INFO("Mux get port from FDB failed for '%s' mac '%s'",
                       nbr.to_string().c_str(), mac.to_string().c_str());
        return true;
    }

    if (!port.empty() && isMuxExists(port))
    {
        MuxCable* ptr = getMuxCable(port);
        return ptr->isActive();
    }

    NextHopKey nh_key = NextHopKey(nbr, alias);
    string curr_port = getNexthopMuxName(nh_key);
    if (port.empty() && !curr_port.empty() && isMuxExists(curr_port))
    {
        MuxCable* ptr = getMuxCable(curr_port);
        return ptr->isActive();
    }

    return true;
}

bool MuxOrch::getMuxPort(const MacAddress& mac, const string& alias, string& portName)
{
    portName = std::string();
    Port rif, port;

    if (!gPortsOrch->getPort(alias, rif))
    {
        SWSS_LOG_ERROR("Interface '%s' not found in port table", alias.c_str());
        return false;
    }

    if (rif.m_type != Port::VLAN)
    {
        SWSS_LOG_DEBUG("Interface type for '%s' is not Vlan, type %d", alias.c_str(), rif.m_type);
        return false;
    }

    if (!gFdbOrch->getPort(mac, rif.m_vlan_info.vlan_id, port))
    {
        SWSS_LOG_INFO("FDB entry not found: Vlan %s, mac %s", alias.c_str(), mac.to_string().c_str());
        return true;
    }

    portName = port.m_alias;
    return true;
}

void MuxOrch::updateFdb(const FdbUpdate& update)
{
    if (!update.add)
    {
        /*
         * For Mac aging, flush events, skip updating mux neighbors.
         * Instead, wait for neighbor update events
         */
        return;
    }

    NeighborEntry neigh;
    MacAddress mac;
    MuxCable* ptr;
    for (auto nh = mux_nexthop_tb_.begin(); nh != mux_nexthop_tb_.end(); ++nh)
    {
        auto res = neigh_orch_->getNeighborEntry(nh->first, neigh, mac);
        if (!res || update.entry.mac != mac)
        {
            continue;
        }

        if (nh->second != update.entry.port_name)
        {
            if (!nh->second.empty() && isMuxExists(nh->second))
            {
                ptr = getMuxCable(nh->second);
                if (ptr->isIpInSubnet(nh->first.ip_address))
                {
                    continue;
                }
                nh->second = update.entry.port_name;
                ptr->updateNeighbor(nh->first, false);
            }

            if (isMuxExists(update.entry.port_name))
            {
                ptr = getMuxCable(update.entry.port_name);
                ptr->updateNeighbor(nh->first, true);
            }
        }
    }
}

void MuxOrch::updateNeighbor(const NeighborUpdate& update)
{
    if (mux_cable_tb_.empty())
    {
        return;
    }

    for (auto it = mux_cable_tb_.begin(); it != mux_cable_tb_.end(); it++)
    {
        MuxCable* ptr = it->second.get();
        if (ptr->isIpInSubnet(update.entry.ip_address))
        {
            ptr->updateNeighbor(update.entry, update.add);
            return;
        }
    }

    string port, old_port;
    if (update.add && !getMuxPort(update.mac, update.entry.alias, port))
    {
        return;
    }
    else if (update.add)
    {
        /* Check if the neighbor already exists */
        old_port = getNexthopMuxName(update.entry);

        /* if new port from FDB is empty or same as existing port, return and
         * no further handling is required
         */
        if (port.empty() || old_port == port)
        {
            addNexthop(update.entry, old_port);
            return;
        }

        addNexthop(update.entry);
    }
    else
    {
        auto it = mux_nexthop_tb_.find(update.entry);
        if (it != mux_nexthop_tb_.end())
        {
            port = it->second;
            removeNexthop(update.entry);
        }
    }

    MuxCable* ptr;
    if (!old_port.empty() && old_port != port && isMuxExists(old_port))
    {
        ptr = getMuxCable(old_port);
        ptr->updateNeighbor(update.entry, false);
        addNexthop(update.entry);
    }

    if (!port.empty() && isMuxExists(port))
    {
        ptr = getMuxCable(port);
        ptr->updateNeighbor(update.entry, update.add);
    }
}

void MuxOrch::addNexthop(NextHopKey nh, string muxName)
{
    mux_nexthop_tb_[nh] = muxName;
}

void MuxOrch::removeNexthop(NextHopKey nh)
{
    mux_nexthop_tb_.erase(nh);
}

string MuxOrch::getNexthopMuxName(NextHopKey nh)
{
    if (mux_nexthop_tb_.find(nh) == mux_nexthop_tb_.end())
    {
        return std::string();
    }

    return mux_nexthop_tb_[nh];
}

sai_object_id_t MuxOrch::getNextHopId(const NextHopKey &nh)
{
    if (mux_nexthop_tb_.find(nh) == mux_nexthop_tb_.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    auto mux_name = mux_nexthop_tb_[nh];
    if (!isMuxExists(mux_name))
    {
        SWSS_LOG_INFO("Mux entry for nh '%s' port '%s' doesn't exist",
                       nh.ip_address.to_string().c_str(), mux_name.c_str());
        return SAI_NULL_OBJECT_ID;
    }

    auto ptr = getMuxCable(mux_name);

    return ptr->getNextHopId(nh);
}

void MuxOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type)
    {
        case SUBJECT_TYPE_NEIGH_CHANGE:
        {
            NeighborUpdate *update = static_cast<NeighborUpdate *>(cntx);
            updateNeighbor(*update);
            break;
        }
        case SUBJECT_TYPE_FDB_CHANGE:
        {
            FdbUpdate *update = static_cast<FdbUpdate *>(cntx);
            updateFdb(*update);
            break;
        }
        default:
            /* Received update in which we are not interested
             * Ignore it
             */
            return;
    }
}

MuxOrch::MuxOrch(DBConnector *db, const std::vector<std::string> &tables,
         TunnelDecapOrch* decapOrch, NeighOrch* neighOrch, FdbOrch* fdbOrch) :
         Orch2(db, tables, request_),
         decap_orch_(decapOrch),
         neigh_orch_(neighOrch),
         fdb_orch_(fdbOrch)
{
    handler_map_.insert(handler_pair(CFG_MUX_CABLE_TABLE_NAME, &MuxOrch::handleMuxCfg));
    handler_map_.insert(handler_pair(CFG_PEER_SWITCH_TABLE_NAME, &MuxOrch::handlePeerSwitch));

    neigh_orch_->attach(this);
    fdb_orch_->attach(this);
}

bool MuxOrch::handleMuxCfg(const Request& request)
{
    SWSS_LOG_ENTER();

    auto srv_ip = request.getAttrIpPrefix("server_ipv4");
    auto srv_ip6 = request.getAttrIpPrefix("server_ipv6");

    std::set<IpAddress> skip_neighbors;

    const auto& port_name = request.getKeyString(0);
    auto op = request.getOperation();

    for (const auto &name : request.getAttrFieldNames())
    {
        if (name == "soc_ipv4")
        {
            auto soc_ip = request.getAttrIpPrefix("soc_ipv4");
            SWSS_LOG_NOTICE("%s: %s was added to ignored neighbor list", port_name.c_str(), soc_ip.getIp().to_string().c_str());
            skip_neighbors.insert(soc_ip.getIp());
        }
        else if (name == "soc_ipv6")
        {
            auto soc_ip6 = request.getAttrIpPrefix("soc_ipv6");
            SWSS_LOG_NOTICE("%s: %s was added to ignored neighbor list", port_name.c_str(), soc_ip6.getIp().to_string().c_str());
            skip_neighbors.insert(soc_ip6.getIp());
        }
    }

    if (op == SET_COMMAND)
    {
        if(isMuxExists(port_name))
        {
            SWSS_LOG_INFO("Mux for port '%s' already exists", port_name.c_str());
            return true;
        }

        if (mux_peer_switch_.isZero())
        {
            SWSS_LOG_INFO("Mux Peer switch addr not yet configured, port '%s'", port_name.c_str());
            return false;
        }

        mux_cable_tb_[port_name] = std::make_unique<MuxCable>
                                   (MuxCable(port_name, srv_ip, srv_ip6, mux_peer_switch_, skip_neighbors));

        SWSS_LOG_NOTICE("Mux entry for port '%s' was added", port_name.c_str());
    }
    else
    {
        if(!isMuxExists(port_name))
        {
            SWSS_LOG_ERROR("Mux for port '%s' does not exists", port_name.c_str());
            return true;
        }

        mux_cable_tb_.erase(port_name);

        SWSS_LOG_NOTICE("Mux cable for port '%s' was removed", port_name.c_str());
    }

    return true;
}

bool MuxOrch::handlePeerSwitch(const Request& request)
{
    SWSS_LOG_ENTER();

    auto peer_ip = request.getAttrIP("address_ipv4");

    const auto& peer_name = request.getKeyString(0);
    auto op = request.getOperation();

    if (op == SET_COMMAND)
    {
        mux_peer_switch_ = peer_ip;

        // Create P2P tunnel when peer_ip is available.
        IpAddresses dst_ips = decap_orch_->getDstIpAddresses(MUX_TUNNEL);
        if (!dst_ips.getSize())
        {
            SWSS_LOG_INFO("Mux tunnel not yet created for '%s' peer ip '%s'",
                           MUX_TUNNEL, peer_ip.to_string().c_str());
            return false;
        }
        auto it =  dst_ips.getIpAddresses().begin();
        const IpAddress& dst_ip = *it;

        // Read dscp_mode of MuxTunnel0 from decap_orch
        string dscp_mode_name = decap_orch_->getDscpMode(MUX_TUNNEL);
        if (dscp_mode_name == "")
        {
            SWSS_LOG_NOTICE("dscp_mode for tunnel %s is not available. Will not be applied", MUX_TUNNEL);
        }

        // Read tc_to_dscp_map_id of MuxTunnel0 from decap_orch
        sai_object_id_t tc_to_dscp_map_id = SAI_NULL_OBJECT_ID;
        decap_orch_->getQosMapId(MUX_TUNNEL, encap_tc_to_dscp_field_name, tc_to_dscp_map_id);
        if (tc_to_dscp_map_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_NOTICE("tc_to_dscp_map_id for tunnel %s is not available. Will not be applied", MUX_TUNNEL);
        }
        // Read tc_to_queue_map_id of MuxTunnel0 from decap_orch
        sai_object_id_t tc_to_queue_map_id = SAI_NULL_OBJECT_ID;
        decap_orch_->getQosMapId(MUX_TUNNEL, encap_tc_to_queue_field_name, tc_to_queue_map_id);
        if (tc_to_queue_map_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_NOTICE("tc_to_queue_map_id for tunnel %s is not available. Will not be applied", MUX_TUNNEL);
        }

        mux_tunnel_id_ = create_tunnel(&peer_ip, &dst_ip, tc_to_dscp_map_id, tc_to_queue_map_id, dscp_mode_name);
        SWSS_LOG_NOTICE("Mux peer ip '%s' was added, peer name '%s'",
                         peer_ip.to_string().c_str(), peer_name.c_str());
    }
    else
    {
        SWSS_LOG_NOTICE("Mux peer ip '%s' delete (Not Implemented), peer name '%s'",
                         peer_ip.to_string().c_str(), peer_name.c_str());
    }

    return true;
}

bool MuxOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    try
    {
        auto& tn = request.getTableName();
        if (handler_map_.find(tn) == handler_map_.end())
        {
            SWSS_LOG_ERROR("Mux %s handler is not initialized", tn.c_str());
            return true;
        }

        return ((this->*(handler_map_[tn]))(request));
    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("Mux add operation error %s ", _.what());
        return true;
    }

    return true;
}

bool MuxOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    try
    {
        auto& tn = request.getTableName();
        if (handler_map_.find(tn) == handler_map_.end())
        {
            SWSS_LOG_ERROR("Mux %s handler is not initialized", tn.c_str());
            return true;
        }

        return ((this->*(handler_map_[tn]))(request));
    }
    catch(std::runtime_error& _)
    {
        SWSS_LOG_ERROR("Mux del operation error %s ", _.what());
        return true;
    }

    return true;
}

MuxCableOrch::MuxCableOrch(DBConnector *db, DBConnector *sdb, const std::string& tableName):
              Orch2(db, tableName, request_),
              app_tunnel_route_table_(db, APP_TUNNEL_ROUTE_TABLE_NAME),
              mux_metric_table_(sdb, STATE_MUX_METRICS_TABLE_NAME)
{
    mux_table_ = unique_ptr<Table>(new Table(db, APP_HW_MUX_CABLE_TABLE_NAME));
}

void MuxCableOrch::updateMuxState(string portName, string muxState)
{
    vector<FieldValueTuple> tuples;
    FieldValueTuple tuple("state", muxState);
    tuples.push_back(tuple);
    mux_table_->set(portName, tuples);
}

void MuxCableOrch::updateMuxMetricState(string portName, string muxState, bool start)
{
    string msg = "orch_switch_" + muxState;
    msg += start? "_start": "_end";

    auto now  = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    auto dur = now - std::chrono::system_clock::from_time_t(now_t);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();

    std::tm now_tm;
    gmtime_r(&now_t, &now_tm);

    char buf[256];
    std::strftime(buf, 256, "%Y-%b-%d %H:%M:%S.", &now_tm);

    /*
     * Prepend '0's for 6 point precision
     */
    const int precision = 6;
    auto ms = to_string(micros);
    if (ms.length() < precision)
    {
        ms.insert(ms.begin(), precision - ms.length(), '0');
    }

    string time = string(buf) + ms;

    mux_metric_table_.hset(portName, msg, time);
}

void MuxCableOrch::addTunnelRoute(const NextHopKey &nhKey)
{
    vector<FieldValueTuple> data;
    string key, alias = nhKey.alias;

    IpPrefix pfx = nhKey.ip_address.to_string();
    key = pfx.to_string();

    FieldValueTuple fvTuple("alias", alias);
    data.push_back(fvTuple);

    SWSS_LOG_INFO("Add tunnel route DB '%s:%s'", alias.c_str(), key.c_str());
    app_tunnel_route_table_.set(key, data);
}

void MuxCableOrch::removeTunnelRoute(const NextHopKey &nhKey)
{
    string key, alias = nhKey.alias;

    IpPrefix pfx = nhKey.ip_address.to_string();
    key = pfx.to_string();

    SWSS_LOG_INFO("Remove tunnel route DB '%s:%s'", alias.c_str(), key.c_str());
    app_tunnel_route_table_.del(key);
}

bool MuxCableOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    if (!mux_orch->isMuxExists(port_name))
    {
        SWSS_LOG_INFO("Mux entry for port '%s' doesn't exist", port_name.c_str());
        return false;
    }

    auto state = request.getAttrString("state");
    auto mux_obj = mux_orch->getMuxCable(port_name);

    try
    {
        mux_obj->setState(state);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Mux Error setting state %s for port %s. Error: %s",
                        state.c_str(), port_name.c_str(), error.what());
        return true;
    }

    SWSS_LOG_NOTICE("Mux State set to %s for port %s", state.c_str(), port_name.c_str());

    return true;
}

bool MuxCableOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    SWSS_LOG_NOTICE("Deleting Mux state entry for port %s not implemented", port_name.c_str());

    return true;
}

MuxStateOrch::MuxStateOrch(DBConnector *db, const std::string& tableName) :
              Orch2(db, tableName, request_),
              mux_state_table_(db, STATE_MUX_CABLE_TABLE_NAME)
{
    SWSS_LOG_ENTER();
}

void MuxStateOrch::updateMuxState(string portName, string muxState)
{
    vector<FieldValueTuple> tuples;
    FieldValueTuple tuple("state", muxState);
    tuples.push_back(tuple);
    mux_state_table_.set(portName, tuples);
}

bool MuxStateOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    if (!mux_orch->isMuxExists(port_name))
    {
        SWSS_LOG_WARN("Mux entry for port '%s' doesn't exist", port_name.c_str());
        return false;
    }

    auto hw_state = request.getAttrString("state");
    auto mux_obj = mux_orch->getMuxCable(port_name);
    string mux_state;

    try
    {
        mux_state = mux_obj->getState();
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Mux error getting state for port %s Error: %s", port_name.c_str(), error.what());
        return false;
    }

    if (mux_obj->isStateChangeInProgress())
    {
        SWSS_LOG_INFO("Mux state change for port '%s' is in-progress", port_name.c_str());
        return false;
    }

    if (mux_state != hw_state)
    {
        if (mux_obj->isStateChangeFailed())
        {
            mux_state = MUX_HW_STATE_ERROR;
        }
        else
        {
            mux_state = MUX_HW_STATE_UNKNOWN;
        }
    }

    SWSS_LOG_NOTICE("Mux setting State DB entry (hw state %s, mux state %s) for port %s",
                     hw_state.c_str(), mux_state.c_str(), port_name.c_str());

    updateMuxState(port_name, mux_state);

    return true;
}

bool MuxStateOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    SWSS_LOG_NOTICE("Deleting state table entry for Mux %s not implemented", port_name.c_str());

    return true;
}
