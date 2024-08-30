#include <memory>

#include "dashenifwdorch.h"
#include "directory.h"
#include <numeric>

extern Directory<Orch*>      gDirectory;

using namespace swss;
using namespace std;

const int EniAclRule::BASE_PRIORITY = 9996;
const std::vector<std::string> EniAclRule::RULE_NAMES = {
    "IN",
    "OUT",
    "IN_TERM",
    "OUT_TERM"
};


DashEniFwdOrch::DashEniFwdOrch(DBConnector* cfgDb, DBConnector* applDb, const std::string& tableName, NeighOrch* neighOrch)
    : Orch2(applDb, tableName, request_), neighorch_(neighOrch)
{
    SWSS_LOG_ENTER();
    acl_table_type_ = make_unique<ProducerStateTable>(applDb, APP_ACL_TABLE_TYPE_TABLE_NAME);
    acl_table_ = make_unique<ProducerStateTable>(applDb, APP_ACL_TABLE_TABLE_NAME);
    ctx = make_shared<EniFwdCtx>(cfgDb, applDb);
    if (neighorch_)
    {
        /* Listen to Neighbor events */
        neighorch_->attach(this);
    }
}

DashEniFwdOrch::~DashEniFwdOrch()
{
    if (neighorch_)
    {
        neighorch_->detach(this);
    }
}

void DashEniFwdOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    switch(type) {
    case SUBJECT_TYPE_NEIGH_CHANGE:
    {
        NeighborUpdate *update = static_cast<NeighborUpdate *>(cntx);
        handleNeighUpdate(*update);
        break;
    }
    default:
        // Ignore the update
        return;
    }
}

void DashEniFwdOrch::handleNeighUpdate(const NeighborUpdate& update)
{
    SWSS_LOG_ENTER();
    auto ipaddr = update.entry.ip_address;
    auto dpu_id_itr = neigh_dpu_map_.find(ipaddr);
    if (dpu_id_itr == neigh_dpu_map_.end())
    {
        return ;
    }
    SWSS_LOG_NOTICE("Neighbor Update: %s, add: %d", ipaddr.to_string().c_str(), update.add);

    auto dpu_id = dpu_id_itr->second;
    auto itr = dpu_eni_map_.lower_bound(dpu_id);
    auto itr_end = dpu_eni_map_.upper_bound(dpu_id);

    while (itr != itr_end)    
    {
        /* Find the eni_itr */
        auto eni_itr = eni_container_.find(itr->second);
        if (eni_itr != eni_container_.end())
        {
            eni_itr->second.update(update);
        }
        itr++;
    }
}

void DashEniFwdOrch::initAclTableCfg()
{
    vector<string> match_list = {
                                  MATCH_TUNNEL_VNI,
                                  MATCH_DST_IP,
                                  MATCH_INNER_SRC_MAC,
                                  MATCH_INNER_DST_MAC,
                                  MATCH_TUNNEL_TERM
                                };

    auto concat = [](const std::string &a, const std::string &b) { return a + "," + b; };

    std::string matches = std::accumulate(
        std::next(match_list.begin()), match_list.end(), match_list[0],
        concat);

    string bpoint_types = string(BIND_POINT_TYPE_PORT) + "," +  string(BIND_POINT_TYPE_PORTCHANNEL);

    vector<FieldValueTuple> fv_ = {
        { ACL_TABLE_TYPE_MATCHES, matches},
        { ACL_TABLE_TYPE_ACTIONS, ACTION_REDIRECT_ACTION },
        { ACL_TABLE_TYPE_BPOINT_TYPES, bpoint_types}
    };

    acl_table_type_->set(ENI_REDIRECT_TABLE_TYPE, fv_);

    auto ports = ctx->getBindPoints();
    std::string ports_str;

    if (!ports.empty())
    {
        ports_str = std::accumulate(std::next(ports.begin()), ports.end(), ports[0], concat);
    }

    /* Write ACL Table */
    vector<FieldValueTuple> table_fv_ = {
        { ACL_TABLE_DESCRIPTION, "Contains Rule for DASH ENI Based Forwarding"},
        { ACL_TABLE_TYPE, ENI_REDIRECT_TABLE_TYPE },
        { ACL_TABLE_STAGE, STAGE_INGRESS },
        { ACL_TABLE_PORTS, ports_str }
    };

    acl_table_->set(ENI_REDIRECT_TABLE, table_fv_);
}

void DashEniFwdOrch::initLocalEndpoints()
{
    auto ids = ctx->dpu_info_.getIds();
    dpu_type_t primary_type = CLUSTER;
    IpAddress local_endp;
    for (auto id : ids)
    {
        if(ctx->dpu_info_.getType(id, primary_type) && primary_type == dpu_type_t::LOCAL)
        {
            if(ctx->dpu_info_.getPaV4(id, local_endp))
            {
                neigh_dpu_map_.insert(make_pair(local_endp, id));
                SWSS_LOG_NOTICE("Local DPU endpoint detected %s", local_endp.to_string().c_str());

                /* Try to resovle the neighbor */
                auto alias = ctx->getNbrAlias(local_endp);
                NextHopKey nh(local_endp, alias);

                if (ctx->isNeighborResolved(nh))
                {
                    SWSS_LOG_WARN("Neighbor already populated.. Not Expected");
                }
                ctx->resolveNeighbor(nh);
            }
        }
    }
}

void DashEniFwdOrch::handleEniDpuMapping(uint64_t id, MacAddress mac, bool add)
{
    /* Make sure id is local */
    dpu_type_t primary_type = CLUSTER;
    if(ctx->dpu_info_.getType(id, primary_type) && primary_type == dpu_type_t::LOCAL)
    {
        if (add)
        {
            dpu_eni_map_.insert(make_pair(id, mac));
        }
        else
        {
            auto range = dpu_eni_map_.equal_range(id);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (it->second == mac)
                {
                    dpu_eni_map_.erase(it);
                    break;
                }
            }
        }
    }
}

void DashEniFwdOrch::lazyInit()
{
    if (ctx_initialized_)
    {
        return ;
    }
    /*
        1. DpuRegistry
        2. Other Orch ptrs
        3. Internal dpu-id mappings
        4. Write ACL Table Cfg
    */
    ctx->initialize();
    ctx->populateDpuRegistry();
    initAclTableCfg();
    initLocalEndpoints();
    ctx_initialized_ = true;
}

bool DashEniFwdOrch::addOperation(const Request& request)
{
    lazyInit();

    bool new_eni = false;
    auto vnet_name = request.getKeyString(0);
    auto eni_id = request.getKeyMacAddress(1);
    auto eni_itr = eni_container_.find(eni_id);

    if (eni_itr == eni_container_.end())
    {
        new_eni = true;
        eni_container_.emplace(std::piecewise_construct,
                            std::forward_as_tuple(eni_id), 
                            std::forward_as_tuple(eni_id.to_string(), vnet_name, ctx));

        eni_itr = eni_container_.find(eni_id);
    }

    if (new_eni)
    {
        eni_itr->second.create(request);
        uint64_t local_ep;
        if (eni_itr->second.findLocalEp(local_ep))
        {
            /* Add to the local map if the endpoint is found */
            handleEniDpuMapping(local_ep, eni_id, true);
        }
    }
    else
    {
        eni_itr->second.update(request);
    }
    return true;
}

bool DashEniFwdOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    auto vnet_name = request.getKeyString(0);
    auto eni_id = request.getKeyMacAddress(1);

    auto eni_itr = eni_container_.find(eni_id);

    if (eni_itr == eni_container_.end())
    {
        SWSS_LOG_ERROR("Invalid del request %s:%s", vnet_name.c_str(), eni_id.to_string().c_str());
        return true;
    }

    bool result = eni_itr->second.destroy(request);
    if (result)
    {
        uint64_t local_ep;
        if (eni_itr->second.findLocalEp(local_ep))
        {
            /* Add to the local map if the endpoint is found */
            handleEniDpuMapping(local_ep, eni_id, false);
        }
    }
    eni_container_.erase(eni_id);
    return true;
}


std::unique_ptr<EniNH> EniNH::createNextHop(dpu_type_t type, const swss::IpAddress& ip)
{
    if (type == dpu_type_t::LOCAL)
    {
        return unique_ptr<EniNH>(new LocalEniNH(ip));
    }
    return unique_ptr<EniNH>(new RemoteEniNH(ip));
}


void LocalEniNH::resolve(EniInfo& eni)
{
    auto& ctx = eni.getCtx();
    auto alias = ctx->getNbrAlias(endpoint_);
    
    NextHopKey nh(endpoint_, alias);
    if (ctx->isNeighborResolved(nh))
    {
        setStatus(endpoint_status_t::RESOLVED);
        return ;
    }

    ctx->resolveNeighbor(nh);
    setStatus(endpoint_status_t::UNRESOLVED);
}


void RemoteEniNH::resolve(EniInfo& eni)
{
    auto& ctx = eni.getCtx();
    auto vnet = eni.getVnet();

    if (!ctx->findVnetTunnel(vnet, tunnel_name_))
    {
        SWSS_LOG_ERROR("Couldn't find tunnel name for Vnet %s", vnet.c_str());
        setStatus(endpoint_status_t::UNRESOLVED);
        return ;
    }

    if (ctx->handleTunnelNH(tunnel_name_, endpoint_, true))
    {
        setStatus(endpoint_status_t::RESOLVED);
    }
    else
    {
        setStatus(endpoint_status_t::UNRESOLVED);
    }
}

void RemoteEniNH::destroy(EniInfo& eni)
{
    auto& ctx = eni.getCtx();
    ctx->handleTunnelNH(tunnel_name_, endpoint_, false);
}


void EniAclRule::setKey(EniInfo& eni)
{
    name_ = string(ENI_REDIRECT_TABLE) + ":" + eni.toKey() + "_" + EniAclRule::RULE_NAMES[type_];
}

update_type_t EniAclRule::processUpdate(EniInfo& eni)
{
    SWSS_LOG_ENTER();
    auto& ctx = eni.getCtx();
    swss::IpAddress primary_endp;
    dpu_type_t primary_type = LOCAL;
    update_type_t update_type = PRIMARY_UPDATE;
    uint64_t primary_id;

    if (type_ == rule_type_t::INBOUND_TERM || type_ == rule_type_t::OUTBOUND_TERM)
    {
        /* Tunnel term entries always use local endpoint regardless of primary id */
        if (!eni.findLocalEp(primary_id))
        {
            SWSS_LOG_ERROR("No Local endpoint was found for Rule: %s", getKey().c_str());
            return update_type_t::INVALID;
        }
    }
    else
    {
        primary_id = eni.getPrimaryId();
    }

    if (!ctx->dpu_info_.getType(primary_id, primary_type))
    {
        SWSS_LOG_ERROR("No primaryId in DPU Table %ld", primary_id);
        return update_type_t::INVALID;
    }

    if (primary_type == LOCAL)
    {
        ctx->dpu_info_.getPaV4(primary_id, primary_endp);
    }
    else
    {
        ctx->dpu_info_.getNpuV4(primary_id, primary_endp);
    }

    if (nh_ == nullptr)
    {
        /* Create Request */
        update_type = update_type_t::CREATE;
    }
    else if (nh_->getType() != primary_type || nh_->getEp() != primary_endp)
    {
        /* primary endpoint is switched */
        update_type = update_type_t::PRIMARY_UPDATE;
        SWSS_LOG_NOTICE("Endpoint IP for Rule %s updated from %s -> %s", getKey().c_str(),
                        nh_->getEp().to_string().c_str(), primary_endp.to_string().c_str());
    }
    else if(nh_->getStatus() == RESOLVED)
    {
        /* No primary update and nexthop resolved, no update
           Neigh Down on a existing local endpoint needs special handling */
        return update_type_t::IDEMPOTENT;
    }

    if (update_type == update_type_t::PRIMARY_UPDATE || update_type == update_type_t::CREATE)
    {
        if (nh_ != nullptr)
        {
            nh_->destroy(eni);
        }
        nh_.reset();
        nh_ = EniNH::createNextHop(primary_type, primary_endp);
    }

    /* Try to resolve the neighbor */
    nh_->resolve(eni);
    return update_type;
}

void EniAclRule::fire(EniInfo& eni)
{
    SWSS_LOG_ENTER();

    auto update_type = processUpdate(eni);

    if (update_type == update_type_t::INVALID || update_type == update_type_t::IDEMPOTENT)
    {
        if (update_type == update_type_t::INVALID)
        {
            setState(rule_state_t::FAILED);
        }
        return ;
    }

    auto& ctx = eni.getCtx();
    auto key = getKey();

    if (state_ == rule_state_t::INSTALLED && update_type == update_type_t::PRIMARY_UPDATE)
    {
        /*  
            Delete the complete rule before updating it, 
            ACLOrch Doesn't support incremental updates 
        */
        ctx->rule_table_->del(key);
        setState(rule_state_t::UNINSTALLED);
        SWSS_LOG_NOTICE("EniFwd ACL Rule %s deleted", key.c_str());
    }

    if (nh_->getStatus() != endpoint_status_t::RESOLVED)
    {
        /* Wait until the endpoint is resolved */
        setState(rule_state_t::PENDING);
        return ;
    }

    vector<FieldValueTuple> fv_ = {
        { RULE_PRIORITY, to_string(BASE_PRIORITY + static_cast<int>(type_)) },
        { MATCH_DST_IP, ctx->getVip().to_string() },
        { getMacMatchDirection(eni), eni.getMac().to_string() },
        { ACTION_REDIRECT_ACTION, nh_->getRedirectVal() }
    };

    if (type_ == rule_type_t::INBOUND_TERM || type_ == rule_type_t::OUTBOUND_TERM)
    {
        fv_.push_back({MATCH_TUNNEL_TERM, "true"});
    }

    if (type_ == rule_type_t::OUTBOUND || type_ == rule_type_t::OUTBOUND_TERM)
    {
        fv_.push_back({MATCH_TUNNEL_VNI, to_string(eni.getOutVni())});
    }
    
    ctx->rule_table_->set(key, fv_);
    setState(INSTALLED);
    SWSS_LOG_NOTICE("EniFwd ACL Rule %s installed", key.c_str());
}

string EniAclRule::getMacMatchDirection(EniInfo& eni)
{
    if (type_ == OUTBOUND || type_ == OUTBOUND_TERM)
    {
        return eni.getOutMacLookup();
    }
    return MATCH_INNER_DST_MAC;
}

void EniAclRule::destroy(EniInfo& eni)
{
    if (state_ == rule_state_t::INSTALLED)
    {
        auto key = getKey();
        auto& ctx = eni.getCtx();
        ctx->rule_table_->del(key);
        if (nh_ != nullptr)
        {
            nh_->destroy(eni);
        }
        nh_.reset();
        setState(rule_state_t::UNINSTALLED);
    }
}


void EniInfo::fireRule(rule_type_t rule_type)
{
    auto rule_itr = rule_container_.find(rule_type);
    if (rule_itr != rule_container_.end())
    {
        rule_itr->second.fire(*this);
    }
}

void EniInfo::fireAllRules()
{
    for (auto& rule_tuple : rule_container_)
    {
        fireRule(rule_tuple.first);
    }
}

bool EniInfo::destroy(const Request& db_request)
{
    for (auto& rule_tuple : rule_container_)
    {
        rule_tuple.second.destroy(*this);
    }
    rule_container_.clear();
    return true;
}

bool EniInfo::create(const Request& db_request)
{
    SWSS_LOG_ENTER();

    auto updates = db_request.getAttrFieldNames();
    auto itr_ep_list = updates.find(ENI_FWD_VDPU_IDS);
    auto itr_primary_id = updates.find(ENI_FWD_PRIMARY);
    auto itr_out_vni = updates.find(ENI_FWD_OUT_VNI);
    auto itr_out_mac_dir = updates.find(ENI_FWD_OUT_MAC_LOOKUP);

    /* Validation Checks */
    if (itr_ep_list == updates.end() || itr_primary_id == updates.end())
    {
        SWSS_LOG_ERROR("Invalid DASH_ENI_FORWARD_TABLE request: No endpoint/primary");
        return false;
    }

    ep_list_ = db_request.getAttrUintList(ENI_FWD_VDPU_IDS);
    primary_id_ = db_request.getAttrUint(ENI_FWD_PRIMARY);

    uint64_t local_id;
    bool tunn_term_allow = findLocalEp(local_id);
    bool outbound_allow = false;

    /* Create Rules */
    rule_container_.emplace(std::piecewise_construct,
                std::forward_as_tuple(rule_type_t::INBOUND),
                std::forward_as_tuple(rule_type_t::INBOUND, *this));
    rule_container_.emplace(std::piecewise_construct,
                std::forward_as_tuple(rule_type_t::OUTBOUND),
                std::forward_as_tuple(rule_type_t::OUTBOUND, *this));

    if (tunn_term_allow)
    {
        /* Create rules for tunnel termination if required */
        rule_container_.emplace(std::piecewise_construct,
                    std::forward_as_tuple(rule_type_t::INBOUND_TERM),
                    std::forward_as_tuple(rule_type_t::INBOUND_TERM, *this));
        rule_container_.emplace(std::piecewise_construct,
                    std::forward_as_tuple(rule_type_t::OUTBOUND_TERM),
                    std::forward_as_tuple(rule_type_t::OUTBOUND_TERM, *this));
    }

    /* Infer Direction to check MAC for outbound rules */
    if (itr_out_mac_dir == updates.end())
    {
        outbound_mac_lookup_ = MATCH_INNER_SRC_MAC;
    }
    else
    {
        auto str = db_request.getAttrString(ENI_FWD_OUT_MAC_LOOKUP);
        if (str == OUT_MAC_DIR)
        {
            outbound_mac_lookup_ = MATCH_INNER_DST_MAC;
        }
        else
        {
            outbound_mac_lookup_ = MATCH_INNER_SRC_MAC;
        }
    }

    /* Infer tunnel_vni for the outbound rules */
    if (itr_out_vni == updates.end())
    {
        if (ctx->findVnetVni(vnet_name_, outbound_vni_))
        {
            outbound_allow = true;
        }
        else
        {
            SWSS_LOG_ERROR("Invalid VNET: No VNI. Cannot install outbound rules: %s", toKey().c_str());
        }
    }
    else
    {
        outbound_vni_ = db_request.getAttrUint(ENI_FWD_OUT_VNI);
        outbound_allow = true;
    }

    fireRule(rule_type_t::INBOUND);

    if (tunn_term_allow)
    {
        fireRule(rule_type_t::INBOUND_TERM);
    }

    if (outbound_allow)
    {
        fireRule(rule_type_t::OUTBOUND);
    }

    if (tunn_term_allow && outbound_allow)
    {
        fireRule(rule_type_t::OUTBOUND_TERM);
    }

    return true;
}

bool EniInfo::update(const NeighborUpdate& nbr_update)
{
    if (nbr_update.add)
    {
        fireAllRules();
    }
    else
    {
        /* 
           Neighbor Delete handling not supported yet
           When this update comes, ACL rule must be deleted first, followed by the NEIGH object
        */
    }
    return true;
}

bool EniInfo::update(const Request& db_request)
{
    SWSS_LOG_ENTER();

    /* Only primary_id is expected to change after ENI is created */
    auto updates = db_request.getAttrFieldNames();
    auto itr_primary_id = updates.find(ENI_FWD_PRIMARY);

    /* Validation Checks */
    if (itr_primary_id == updates.end())
    {
        throw std::logic_error("Invalid DASH_ENI_FORWARD_TABLE update: No primary idx");
    }

    if (getPrimaryId() == db_request.getAttrUint(ENI_FWD_PRIMARY))
    {
        /* No update in the primary id, return true */
        return true;
    }

    /* Update local primary id and fire the rules */
    primary_id_ = db_request.getAttrUint(ENI_FWD_PRIMARY);
    fireAllRules();

    return true;
}

bool EniInfo::findLocalEp(uint64_t& local_endpoint) const
{
    /* Check if atleast one of the endpoints is local */
    bool found = false;
    for (auto idx : ep_list_)
    {   
        dpu_type_t val = dpu_type_t::EXTERNAL;
        if (ctx->dpu_info_.getType(idx, val) && val == dpu_type_t::LOCAL)
        {
            if (!found)
            {
                found = true;
                local_endpoint = idx;
            }
            else
            {
                SWSS_LOG_WARN("Multiple Local Endpoints for the ENI %s found, proceeding with %ld",
                                mac_.to_string().c_str(), local_endpoint);
            }
        }
    }
    return found;
}

void EniInfo::formatMac()
{
    /* f4:93:9f:ef:c4:7e -> F4939FEFC47E */
    mac_key_.clear();
    auto mac_orig = mac_.to_string();
    for (char c : mac_orig) {
        if (c != ':') { // Skip colons
            mac_key_ += static_cast<char>(std::toupper(c));
        }
    }
}


void DpuRegistry::populate(Table* dpuTable)
{
    SWSS_LOG_ENTER();
    std::vector<std::string> keys;
    dpuTable->getKeys(keys);

    for (auto key : keys)
    {
        try
        {
            std::vector<FieldValueTuple> values;
            dpuTable->get(key, values);

            KeyOpFieldsValuesTuple kvo = {
                key, SET_COMMAND, values
            };
            processDpuTable(kvo);
        }
        catch(exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse key:%s in the %s", key.c_str(), CFG_DPU_TABLE);
        }
    }

    SWSS_LOG_INFO("DPU data read. %ld dpus found", dpus_.size());
}

void DpuRegistry::processDpuTable(const KeyOpFieldsValuesTuple& kvo)
{
    DpuData data;

    dpu_request_.clear();
    dpu_request_.parse(kvo);
    
    uint64_t key = dpu_request_.getKeyUint(0);
    string type = dpu_request_.getAttrString(DPU_TYPE);

    dpus_ids_.push_back(key);

    if (type == "local")
    {
        data.type = dpu_type_t::LOCAL;
    }
    else
    {
        // External type is not suported
        data.type = dpu_type_t::CLUSTER;
    }

    data.state = dpu_request_.getAttrString(DPU_STATE);
    data.pa_v4 = dpu_request_.getAttrIP(DPU_PA_V4);
    data.npu_v4 = dpu_request_.getAttrIP(DPU_NPU_V4);
    dpus_.insert({key, data});
}


EniFwdCtxBase::EniFwdCtxBase(DBConnector* cfgDb, DBConnector* applDb)
{
    dpu_tbl_ = make_unique<Table>(cfgDb, CFG_DPU_TABLE);
    port_tbl_ = make_unique<Table>(cfgDb, CFG_PORT_TABLE_NAME);
    vip_tbl_ = make_unique<Table>(cfgDb, CFG_VIP_TABLE_TMP);
    rule_table_ = make_unique<ProducerStateTable>(applDb, APP_ACL_RULE_TABLE_NAME);
    vip_inferred_ = false;
}

std::set<std::string> EniFwdCtxBase::findInternalPorts()
{
    std::vector<std::string> all_ports;
    std::set<std::string> internal_ports;
    port_tbl_->getKeys(all_ports);
    for (auto& port : all_ports)
    {
        std::string val;
        if (port_tbl_->hget(port, PORT_ROLE, val))
        {
            if (val == PORT_ROLE_DPC)
            {
                internal_ports.insert(port);
            }
        }
    }
    return internal_ports;
}

vector<string> EniFwdCtxBase::getBindPoints()
{
    std::vector<std::string> bpoints;
    auto internal_ports = findInternalPorts();
    auto all_ports = getAllPorts();

    std::set<std::string> legitSet;

    /* Add Phy and Lag ports */
    for (auto &it: all_ports)
    {
        if (it.second.m_type == Port::PHY || it.second.m_type == Port::LAG)
        {
            legitSet.insert(it.first);
        }
    }

    /* Remove any Lag Members PHY's */
    for (auto &it: all_ports)
    {
        Port& port = it.second;
        if (port.m_type == Port::LAG)
        {
            for (auto mem : port.m_members)
            {
                /* Remove any members that are part of a LAG */
                legitSet.erase(mem);
            }
        }
    }

    /* Filter Internal ports */
    for (auto& port : legitSet)
    {
        if (internal_ports.find(port) == internal_ports.end())
        {
            bpoints.push_back(port);
        }
    }

    return bpoints;
}

string EniFwdCtxBase::getNbrAlias(const swss::IpAddress& nh_ip)
{
    auto itr = nh_alias_map_.find(nh_ip);
    if (itr != nh_alias_map_.end())
    {
        return itr->second;
    }

    auto alias = this->getRouterIntfsAlias(nh_ip);
    if (!alias.empty())
    {
        nh_alias_map_.insert(std::pair<IpAddress, string>(nh_ip, alias));
    }
    return alias;
}

bool EniFwdCtxBase::handleTunnelNH(const std::string& tunnel_name, swss::IpAddress endpoint, bool create)
{
    SWSS_LOG_ENTER();

    auto nh_key = endpoint.to_string() + "@" + tunnel_name;
    auto nh_itr = remote_nh_map_.find(nh_key);

    /* Delete Tunnel NH if ref_count = 0 */
    if (!create)
    {
        if (nh_itr != remote_nh_map_.end())
        {
            if(!--nh_itr->second.first)
            {
                remote_nh_map_.erase(nh_key);
                return removeNextHopTunnel(tunnel_name, endpoint);
            }
        }
        return true;
    }

    if (nh_itr == remote_nh_map_.end())
    {
        /* Create a tunnel NH */
        auto nh_oid = createNextHopTunnel(tunnel_name, endpoint);
        if (nh_oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create Tunnel Next Hop, name: %s. endpoint %s", tunnel_name.c_str(),
                            endpoint.to_string().c_str());
        }
        remote_nh_map_.insert(make_pair(nh_key, make_pair(0, nh_oid)));
        nh_itr = remote_nh_map_.find(nh_key);
    }
    nh_itr->second.first++; /* Increase the ref count, indicates number of rules using referencing this */
    return true;
}

IpPrefix EniFwdCtxBase::getVip()
{
    SWSS_LOG_ENTER();

    if (!vip_inferred_)
    {
        std::vector<std::string> keys;
        vip_tbl_->getKeys(keys);
        if (keys.empty())
        {
            SWSS_LOG_THROW("Invalid Config: VIP info not populated");
        }

        try
        {
            vip = IpPrefix(keys[0]);
            SWSS_LOG_NOTICE("VIP found: %s", vip.to_string().c_str());
        }
        catch (std::exception& e)
        {
            SWSS_LOG_THROW("VIP is not formatted correctly %s",  keys[0].c_str());
        }
        vip_inferred_ = true;
    }
    return vip;
}


void EniFwdCtx::initialize()
{
    portsorch_ = gDirectory.get<PortsOrch*>();
    neighorch_ = gDirectory.get<NeighOrch*>();
    intfsorch_ = gDirectory.get<IntfsOrch*>();
    vnetorch_ = gDirectory.get<VNetOrch*>();
    vxlanorch_ = gDirectory.get<VxlanTunnelOrch*>();
    assert(portsorch_);
    assert(neighorch_);
    assert(intfsorch_);
    assert(vnetorch_);
    assert(vxlanorch_);
}

sai_object_id_t EniFwdCtx::createNextHopTunnel(string tunnel_name, IpAddress ip_addr)
{
    return safetyWrapper(vxlanorch_, &VxlanTunnelOrch::createNextHopTunnel,
                        (sai_object_id_t)SAI_NULL_OBJECT_ID,
                        (string)tunnel_name, ip_addr,
                        MacAddress(),
                        (uint32_t)0);
}

bool EniFwdCtx::removeNextHopTunnel(string tunnel_name, IpAddress ip_addr)
{
    return safetyWrapper(vxlanorch_, &VxlanTunnelOrch::removeNextHopTunnel,
                        false,
                        (std::string)tunnel_name, ip_addr,
                        MacAddress(),
                        (uint32_t)0);
}
