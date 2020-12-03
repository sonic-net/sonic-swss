#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <inttypes.h>

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

/* Global variables */
extern Directory<Orch*> gDirectory;
extern CrmOrch *gCrmOrch;
extern NeighOrch *gNeighOrch;
extern AclOrch *gAclOrch;
extern PortsOrch *gPortsOrch;

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_object_id_t gSwitchId;
extern sai_route_api_t* sai_route_api;
extern sai_tunnel_api_t* sai_tunnel_api;
extern sai_next_hop_api_t* sai_next_hop_api;
extern sai_router_interface_api_t* sai_router_intfs_api;

/* Constants */
#define MUX_TUNNEL "MUX_TUNNEL0"
#define MUX_ACL_TABLE_NAME "mux_acl_table";
#define MUX_ACL_RULE_NAME "mux_acl_rule";
#define MUX_HW_STATE_UNKNOWN "unknown"
#define MUX_HW_STATE_PENDING "pending"

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
        SWSS_LOG_ERROR("Failed to create tunnel route %s, rv:%d",
                pfx.getIp().to_string().c_str(), status);
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

static sai_object_id_t create_tunnel(const IpAddress* p_dst_ip, const IpAddress* p_src_ip)
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

MuxCable::MuxCable(string name, IpPrefix& srv_ip4, IpPrefix& srv_ip6, IpAddress peer_ip)
         :mux_name_(name), srv_ip4_(srv_ip4), srv_ip6_(srv_ip6), peer_ip4_(peer_ip)
{
    mux_orch_ = gDirectory.get<MuxOrch*>();
    mux_cb_orch_ = gDirectory.get<MuxCableOrch*>();
    mux_state_orch_ = gDirectory.get<MuxStateOrch*>();

    nbr_handler_ = unique_ptr<MuxNbrHandler> (new MuxNbrHandler());

    state_machine_handlers_.insert(handler_pair(MUX_STATE_INIT_ACTIVE, &MuxCable::stateInitActive));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_STANDBY_ACTIVE, &MuxCable::stateActive));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_INIT_STANDBY, &MuxCable::stateStandby));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_ACTIVE_STANDBY, &MuxCable::stateStandby));
}

bool MuxCable::stateInitActive()
{
    SWSS_LOG_INFO("Set state to Active from %s", muxStateValToString.at(state_).c_str());

    if (!nbrHandler())
    {
        return false;
    }

    return true;
}

bool MuxCable::stateActive()
{
    SWSS_LOG_INFO("Set state to Active from %s", muxStateValToString.at(state_).c_str());

    Port port;
    if (!gPortsOrch->getPort(mux_name_, port))
    {
        SWSS_LOG_NOTICE("Port %s not found in port table", mux_name_.c_str());
        return false;
    }

    if (!aclHandler(port.m_port_id, false))
    {
        SWSS_LOG_INFO("Remove ACL drop rule failed for %s", mux_name_.c_str());
        return false;
    }

    if (!nbrHandler())
    {
        return false;
    }

    if (remove_route(srv_ip4_) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    if (remove_route(srv_ip6_) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    mux_orch_->removeNextHopTunnel(MUX_TUNNEL, peer_ip4_);

    return true;
}

bool MuxCable::stateStandby()
{
    SWSS_LOG_INFO("Set state to Standby from %s", muxStateValToString.at(state_).c_str());

    Port port;
    if (!gPortsOrch->getPort(mux_name_, port))
    {
        SWSS_LOG_NOTICE("Port %s not found in port table", mux_name_.c_str());
        return false;
    }

    sai_object_id_t nh = mux_orch_->createNextHopTunnel(MUX_TUNNEL, peer_ip4_);

    if (nh == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Null NH object id, retry for %s", peer_ip4_.to_string().c_str());
        return false;
    }

    if (create_route(srv_ip4_, nh) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    if (create_route(srv_ip6_, nh) != SAI_STATUS_SUCCESS)
    {
        remove_route(srv_ip4_);
        return false;
    }

    if (!nbrHandler(false))
    {
        remove_route(srv_ip4_);
        remove_route(srv_ip6_);
        return false;
    }

    if (!aclHandler(port.m_port_id))
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

    auto it = muxStateTransition.find(make_pair(state_, ns));

    if (it ==  muxStateTransition.end())
    {
        SWSS_LOG_ERROR("State transition from %s to %s is not-handled ",
                        muxStateValToString.at(state_).c_str(), new_state.c_str());
        return;
    }

    mux_cb_orch_->updateMuxState(mux_name_, new_state);

    MuxState state = state_;
    state_ = ns;

    st_chg_in_progress_ = true;

    if (!(this->*(state_machine_handlers_[it->second]))())
    {
        //Reset back to original state
        state_ = state;
        st_chg_in_progress_ = false;
        throw std::runtime_error("Failed to handle state transition");
    }

    st_chg_in_progress_ = false;
    SWSS_LOG_INFO("Changed state to %s", new_state.c_str());

    return;
}

string MuxCable::getState()
{
    SWSS_LOG_INFO("Get state request for %s, state %s",
                   mux_name_.c_str(), muxStateValToString.at(state_).c_str());

    return (muxStateValToString.at(state_));
}

bool MuxCable::aclHandler(sai_object_id_t port, bool add)
{
    if (add)
    {
        acl_handler_ = make_shared<MuxAclHandler>(port);
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

bool MuxCable::nbrHandler(bool enable)
{
    if (enable)
    {
        return nbr_handler_->enable();
    }
    else
    {
        return nbr_handler_->disable();
    }
}

void MuxNbrHandler::update(IpAddress ip, string alias, bool add)
{
    if (add)
    {
        neighbors_.add(ip);
        if (!alias.empty() && alias != alias_)
        {
            alias_ = alias;
        }
    }
    else
    {
        neighbors_.remove(ip);
    }
}

bool MuxNbrHandler::enable()
{
    NeighborEntry neigh;

    auto it = neighbors_.getIpAddresses().begin();
    while (it != neighbors_.getIpAddresses().end())
    {
        neigh = NeighborEntry(*it, alias_);
        if (!gNeighOrch->enableNeighbor(neigh))
        {
            return false;
        }
        it++;
    }

    return true;
}

bool MuxNbrHandler::disable()
{
    NeighborEntry neigh;

    auto it = neighbors_.getIpAddresses().begin();
    while (it != neighbors_.getIpAddresses().end())
    {
        neigh = NeighborEntry(*it, alias_);
        if (!gNeighOrch->disableNeighbor(neigh))
        {
            return false;
        }
        it++;
    }

    return true;
}

std::map<std::string, AclTable> MuxAclHandler::acl_table_;

MuxAclHandler::MuxAclHandler(sai_object_id_t port)
{
    SWSS_LOG_ENTER();

    // There is one handler instance per MUX port
    acl_table_type_t table_type = ACL_TABLE_MUX;
    string table_name = MUX_ACL_TABLE_NAME;
    string rule_name = MUX_ACL_RULE_NAME;

    port_ = port;
    auto found = acl_table_.find(table_name);
    if (found == acl_table_.end())
    {
        SWSS_LOG_NOTICE("First time create for port %lx", port);

        // First time handling of Mux Table, create ACL table, and bind
        createMuxAclTable(port, table_name);
        shared_ptr<AclRuleMux> newRule =
                make_shared<AclRuleMux>(gAclOrch, rule_name, table_name, table_type);
        createMuxAclRule(newRule, table_name);
    }
    else
    {
        SWSS_LOG_NOTICE("Binding port %lx", port);
        // Otherwise just bind ACL table with the port
        found->second.bind(port);
    }
}

MuxAclHandler::~MuxAclHandler(void)
{
    SWSS_LOG_ENTER();
    string table_name = MUX_ACL_TABLE_NAME;

    SWSS_LOG_NOTICE("Un-Binding port %lx", port_);

    auto found = acl_table_.find(table_name);
    found->second.unbind(port_);
}

void MuxAclHandler::createMuxAclTable(sai_object_id_t port, string strTable)
{
    SWSS_LOG_ENTER();

    auto inserted = acl_table_.emplace(piecewise_construct,
                                       std::forward_as_tuple(strTable),
                                       std::forward_as_tuple());

    assert(inserted.second);

    AclTable& acl_table = inserted.first->second;
    acl_table.type = ACL_TABLE_MUX;
    acl_table.id = strTable;
    acl_table.link(port);
    acl_table.stage = ACL_STAGE_INGRESS;
    gAclOrch->addAclTable(acl_table);
}

void MuxAclHandler::createMuxAclRule(shared_ptr<AclRuleMux> rule, string strTable)
{
    SWSS_LOG_ENTER();

    string attr_name, attr_value;

    attr_name = RULE_PRIORITY;
    attr_value = "999";
    rule->validateAddPriority(attr_name, attr_value);

    attr_name = ACTION_PACKET_ACTION;
    attr_value = PACKET_ACTION_DROP;
    rule->validateAddAction(attr_name, attr_value);

    gAclOrch->addAclRule(rule, strTable);
}

sai_object_id_t MuxOrch::createNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr)
{
    auto it = mux_tunnel_nh_.find(ipAddr);
    if (it != mux_tunnel_nh_.end())
    {
        ++it->second.ref_count;
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

bool MuxOrch::isNeighborActive(IpAddress nbr, string alias)
{
    MuxCable* ptr = findMuxCableInSubnet(nbr);

    if (ptr)
    {
        return ptr->isActive();
    }

    return true;
}

void MuxOrch::updateNeighbor(const NeighborUpdate& update)
{
    for (auto it = mux_cable_tb_.begin(); it != mux_cable_tb_.end(); it++)
    {
        MuxCable* ptr = it->second.get();
        if (ptr->isIpInSubnet(update.entry.ip_address))
        {
            ptr->updateNeighbor(update.entry.ip_address, update.entry.alias, update.add);
        }
    }
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
        default:
            /* Received update in which we are not interested
             * Ignore it
             */
            return;
    }
}

MuxOrch::MuxOrch(DBConnector *db, const std::vector<std::string> &tables, TunnelDecapOrch* decapOrch, NeighOrch* neighOrch)
          : Orch2(db, tables, request_), decap_orch_(decapOrch), neigh_orch_(neighOrch)
{
    handler_map_.insert(handler_pair(CFG_MUX_CABLE_TABLE_NAME, &MuxOrch::handleMuxCfg));
    handler_map_.insert(handler_pair(CFG_PEER_SWITCH_TABLE_NAME, &MuxOrch::handlePeerSwitch));

    neigh_orch_->attach(this);
}

bool MuxOrch::handleMuxCfg(const Request& request)
{
    SWSS_LOG_ENTER();

    auto srv_ip = request.getAttrIpPrefix("server_ipv4");
    auto srv_ip6 = request.getAttrIpPrefix("server_ipv6");

    const auto& port_name = request.getKeyString(0);
    auto op = request.getOperation();

    if (op == SET_COMMAND)
    {
        if(isMuxExists(port_name))
        {
            SWSS_LOG_ERROR("Mux for port '%s' already exists", port_name.c_str());
            return true;
        }

        if (mux_peer_switch_.isZero())
        {
            SWSS_LOG_ERROR("Peer switch addr not yet configured, port '%s'", port_name.c_str());
            return false;
        }

        mux_cable_tb_[port_name] = std::unique_ptr<MuxCable>
                                   (new MuxCable(port_name, srv_ip, srv_ip6, mux_peer_switch_));

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
        // Create P2P tunnel when peer_ip is available.
        IpAddresses dst_ips = decap_orch_->getDstIpAddresses(MUX_TUNNEL);
        if (!dst_ips.getSize())
        {
            SWSS_LOG_NOTICE("Mux tunnel not yet created for '%s' peer ip '%s'",
                            MUX_TUNNEL, peer_ip.to_string().c_str());
            return false;
        }

        auto it =  dst_ips.getIpAddresses().begin();
        const IpAddress& dst_ip = *it;
        mux_tunnel_id_ = create_tunnel(&peer_ip, &dst_ip);
        mux_peer_switch_ = peer_ip;
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
            SWSS_LOG_ERROR(" %s handler is not initialized", tn.c_str());
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
            SWSS_LOG_ERROR(" %s handler is not initialized", tn.c_str());
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

MuxCableOrch::MuxCableOrch(DBConnector *db, const std::string& tableName): Orch2(db, tableName, request_)
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

bool MuxCableOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    if (!mux_orch->isMuxExists(port_name))
    {
        SWSS_LOG_WARN("Mux entry for port '%s' doesn't exist", port_name.c_str());
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
        SWSS_LOG_ERROR("Error setting state %s for port %s. Error: %s",
                        state.c_str(), port_name.c_str(), error.what());
        return false;
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
        SWSS_LOG_ERROR("Error getting state for port %s Error: %s", port_name.c_str(), error.what());
        return false;
    }

    if (mux_obj->isStateChangeInProgress())
    {
        SWSS_LOG_NOTICE("Mux state change for port '%s' is in-progress", port_name.c_str());
        return false;
    }

    if (mux_state != hw_state)
    {
        mux_state = MUX_HW_STATE_UNKNOWN;
    }

    SWSS_LOG_NOTICE("Setting State DB entry (hw state %s, mux state %s) for port %s",
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
