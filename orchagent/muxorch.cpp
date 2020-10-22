#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <inttypes.h>

#include "sai.h"
#include "ipaddress.h"
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
extern sai_object_id_t gSwitchId;
extern sai_route_api_t* sai_route_api;

/* Constants */
#define MUX_TUNNEL "MUX_TUNNEL"
#define MUX_ACL_TABLE_NAME "mux_acl_table";
#define MUX_ACL_RULE_NAME "mux_acl_rule";
#define MUX_HW_STATE_FAILED "failed"

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
};

const map <string, MuxState> muxStateStringToVal =
{
    { "active", MuxState::MUX_STATE_ACTIVE },
    { "standby", MuxState::MUX_STATE_STANDBY },
    { "init", MuxState::MUX_STATE_INIT },
    { "failed", MuxState::MUX_STATE_FAILED },
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

MuxCable::MuxCable(string name, IpPrefix& srv_ip4, IpPrefix& srv_ip6, IpAddress peer_ip)
         :mux_name_(name), srv_ip4_(srv_ip4), srv_ip6_(srv_ip6), peer_ip4_(peer_ip)
{
    mux_cfg_orch_ = gDirectory.get<MuxCfgOrch*>();
    mux_orch_ = gDirectory.get<MuxOrch*>();
    mux_state_orch_ = gDirectory.get<MuxStateOrch*>();

    state_machine_handlers_.insert(handler_pair(MUX_STATE_INIT_ACTIVE, &MuxCable::stateInitActive));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_STANDBY_ACTIVE, &MuxCable::stateActive));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_INIT_STANDBY, &MuxCable::stateStandby));
    state_machine_handlers_.insert(handler_pair(MUX_STATE_ACTIVE_STANDBY, &MuxCable::stateStandby));
}

bool MuxCable::stateInitActive()
{
    SWSS_LOG_INFO("Set state to Active from %s", muxStateValToString.at(state_).c_str());

    NeighborEntry neigh = NeighborEntry(srv_ip4_.getIp().to_string());
    if (!gNeighOrch->enableNeighbor(neigh))
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

    NeighborEntry neigh = NeighborEntry(srv_ip4_.getIp().to_string());
    if (!gNeighOrch->enableNeighbor(neigh))
    {
        return false;
    }

    if (remove_route(srv_ip4_) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    mux_cfg_orch_->removeNextHopTunnel(MUX_TUNNEL, peer_ip4_);

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

    sai_object_id_t nh = mux_cfg_orch_->createNextHopTunnel(MUX_TUNNEL, peer_ip4_);

    if (nh == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Null NH object id, retry for %s", peer_ip4_.to_string().c_str());
        return false;
    }

    if (create_route(srv_ip4_, nh) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    NeighborEntry neigh = NeighborEntry(srv_ip4_.getIp().to_string());

    if (!gNeighOrch->disableNeighbor(neigh))
    {
        remove_route(srv_ip4_);
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
    SWSS_LOG_NOTICE("Set state to %s from %s",
                     new_state.c_str(), muxStateValToString.at(state_).c_str());

    MuxState ns = muxStateStringToVal.at(new_state);

    auto it = muxStateTransition.find(make_pair(state_, ns));

    if (it ==  muxStateTransition.end())
    {
        SWSS_LOG_ERROR("State transition from %s to %s is not-handled ",
                        muxStateValToString.at(state_).c_str(), new_state.c_str());
        return;
    }

    mux_orch_->updateMuxState(mux_name_, new_state);

    if (!(this->*(state_machine_handlers_[it->second]))())
    {
        throw std::runtime_error("Failed to handle state transition");
    }

    state_ = ns;

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

sai_object_id_t MuxCfgOrch::createNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr)
{
    auto it = mux_tunnel_nh_.find(ipAddr);
    if (it != mux_tunnel_nh_.end())
    {
        ++it->second.ref_count;
        return it->second.nh_id;
    }

    sai_object_id_t nh = decap_orch_->createNextHopTunnel(tunnelKey, ipAddr);

    if (SAI_NULL_OBJECT_ID != nh)
    {
        mux_tunnel_nh_[ipAddr] = { nh, 1 };
    }

    return nh;
}

bool MuxCfgOrch::removeNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr)
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
        mux_tunnel_nh_.erase(ipAddr);

        if (!decap_orch_-> removeNextHopTunnel(tunnelKey, ipAddr))
        {
            SWSS_LOG_ERROR("NH tunnel remove failed %s, ip %s",
                            tunnelKey.c_str(), ipAddr.to_string().c_str());
            return false;
        }
    }

    SWSS_LOG_INFO("NH tunnel removed  %s, ip %s or decremented to ref count %d",
                   tunnelKey.c_str(), ipAddr.to_string().c_str(), ref_cnt);
    return true;
}


MuxCfgOrch::MuxCfgOrch(DBConnector *db, const std::vector<std::string> &tables, TunnelDecapOrch* decapOrch)
          : Orch2(db, tables, request_), decap_orch_(decapOrch)
{
    handler_map_.insert(handler_pair(CFG_MUX_CABLE_TABLE_NAME, &MuxCfgOrch::handleMuxCfg));
    handler_map_.insert(handler_pair(CFG_PEER_SWITCH_TABLE_NAME, &MuxCfgOrch::handlePeerSwitch));
}

bool MuxCfgOrch::handleMuxCfg(const Request& request)
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
            SWSS_LOG_ERROR("Mux for port '%s' is already exists", port_name.c_str());
            return true;
        }

        if (mux_peer_switch_.isZero())
        {
            SWSS_LOG_ERROR("Peer switch address '%s' not yet configured", port_name.c_str());
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

bool MuxCfgOrch::handlePeerSwitch(const Request& request)
{
    SWSS_LOG_ENTER();

    auto peer_ip = request.getAttrIP("address_ipv4");

    const auto& peer_name = request.getKeyString(0);
    auto op = request.getOperation();

    if (op == SET_COMMAND)
    {
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

bool MuxCfgOrch::addOperation(const Request& request)
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

bool MuxCfgOrch::delOperation(const Request& request)
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

MuxOrch::MuxOrch(DBConnector *db, const std::string& tableName): Orch2(db, tableName, request_)
{
    mux_table_ = unique_ptr<Table>(new Table(db, APP_HW_MUX_CABLE_TABLE_NAME));
}

void MuxOrch::updateMuxState(string portName, string muxState)
{
    vector<FieldValueTuple> tuples;
    FieldValueTuple tuple("state", muxState);
    tuples.push_back(tuple);
    mux_table_->set(portName, tuples);
}

bool MuxOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    MuxCfgOrch* mux_cfg_orch = gDirectory.get<MuxCfgOrch*>();
    if (!mux_cfg_orch->isMuxExists(port_name))
    {
        SWSS_LOG_WARN("Mux entry for port '%s' doesn't exist", port_name.c_str());
        return false;
    }

    auto state = request.getAttrString("state");
    auto mux_obj = mux_cfg_orch->getMuxCable(port_name);

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

    SWSS_LOG_NOTICE("State set to %s for port %s", state.c_str(), port_name.c_str());

    return true;
}

bool MuxOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto port_name = request.getKeyString(0);

    SWSS_LOG_NOTICE("Deleting state entry for port %s not implemented", port_name.c_str());

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

    MuxCfgOrch* mux_cfg_orch = gDirectory.get<MuxCfgOrch*>();
    if (!mux_cfg_orch->isMuxExists(port_name))
    {
        SWSS_LOG_WARN("Mux entry for port '%s' doesn't exist", port_name.c_str());
        return false;
    }

    auto hw_state = request.getAttrString("state");
    auto mux_obj = mux_cfg_orch->getMuxCable(port_name);
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

    if (mux_state != hw_state)
    {
        mux_state = MUX_HW_STATE_FAILED;
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
