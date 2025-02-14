#include "dashenifwdorch.h"

using namespace swss;
using namespace std;

const int EniAclRule::BASE_PRIORITY = 9996;
const vector<string> EniAclRule::RULE_NAMES = {
    "IN",
    "OUT",
    "IN_TERM",
    "OUT_TERM"
};

unique_ptr<EniNH> EniNH::createNextHop(dpu_type_t type, const IpAddress& ip)
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

string LocalEniNH::getRedirectVal() 
{ 
    return endpoint_.to_string(); 
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

string RemoteEniNH::getRedirectVal() 
{ 
    return endpoint_.to_string() + "@" + tunnel_name_; 
}

void EniAclRule::setKey(EniInfo& eni)
{
    name_ = string(ENI_REDIRECT_TABLE) + ":" + eni.toKey() + "_" + EniAclRule::RULE_NAMES[type_];
}

update_type_t EniAclRule::processUpdate(EniInfo& eni)
{
    SWSS_LOG_ENTER();
    auto& ctx = eni.getCtx();
    IpAddress primary_endp;
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

    if (!ctx->dpu_info.getType(primary_id, primary_type))
    {
        SWSS_LOG_ERROR("No primaryId in DPU Table %" PRIu64 "", primary_id);
        return update_type_t::INVALID;
    }

    if (primary_type == LOCAL)
    {
        ctx->dpu_info.getPaV4(primary_id, primary_endp);
    }
    else
    {
        ctx->dpu_info.getNpuV4(primary_id, primary_endp);
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
    /*
        Process an ENI update and handle the ACL rule accordingly
        1) See if the update is valid and infer if the nexthop is local or remote
        2) Create a NextHop object and if resolved, proceed with installing the ACL Rule
    */
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
        ctx->rule_table->del(key);
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
    
    ctx->rule_table->set(key, fv_);
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
        ctx->rule_table->del(key);
        if (nh_ != nullptr)
        {
            nh_->destroy(eni);
        }
        nh_.reset();
        setState(rule_state_t::UNINSTALLED);
    }
}

void EniAclRule::setState(rule_state_t state)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("EniFwd ACL Rule: %s State Change %d -> %d", getKey().c_str(), state_, state);
    state_ = state;
}


EniInfo::EniInfo(const string& mac_str, const string& vnet, const shared_ptr<EniFwdCtxBase>& ctx) :
    mac_(mac_str), vnet_name_(vnet), ctx(ctx)
{
    formatMac(); 
}

string EniInfo::toKey() const
{
    return vnet_name_ + "_" + mac_key_;
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
    rule_container_.emplace(piecewise_construct,
                forward_as_tuple(rule_type_t::INBOUND),
                forward_as_tuple(rule_type_t::INBOUND, *this));
    rule_container_.emplace(piecewise_construct,
                forward_as_tuple(rule_type_t::OUTBOUND),
                forward_as_tuple(rule_type_t::OUTBOUND, *this));

    if (tunn_term_allow)
    {
        /* Create rules for tunnel termination if required */
        rule_container_.emplace(piecewise_construct,
                    forward_as_tuple(rule_type_t::INBOUND_TERM),
                    forward_as_tuple(rule_type_t::INBOUND_TERM, *this));
        rule_container_.emplace(piecewise_construct,
                    forward_as_tuple(rule_type_t::OUTBOUND_TERM),
                    forward_as_tuple(rule_type_t::OUTBOUND_TERM, *this));
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
        throw logic_error("Invalid DASH_ENI_FORWARD_TABLE update: No primary idx");
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
        if (ctx->dpu_info.getType(idx, val) && val == dpu_type_t::LOCAL)
        {
            if (!found)
            {
                found = true;
                local_endpoint = idx;
            }
            else
            {
                SWSS_LOG_WARN("Multiple Local Endpoints for the ENI %s found, proceeding with %" PRIu64 ""  ,
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
            mac_key_ += static_cast<char>(toupper(c));
        }
    }
}
