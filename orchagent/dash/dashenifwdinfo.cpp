#include "dashenifwdorch.h"

using namespace swss;
using namespace std;

const int EniAclRule::BASE_PRIORITY = 9996;

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

    uint64_t vnet_vni;
    if (!ctx->findVnetVni(vnet, vnet_vni))
    {
        SWSS_LOG_ERROR("Couldn't find VNI for Vnet %s", vnet.c_str());
        setStatus(endpoint_status_t::UNRESOLVED);
        return ;
    }

    vni_ = std::to_string(vnet_vni);

    /* Note: AclOrch already has logic to create / delete Tunnel NH, no need to create here */
    setStatus(endpoint_status_t::RESOLVED);
}

string RemoteEniNH::getRedirectVal() 
{ 
    /* Format Expected by AclOrch: endpoint_ip@tunnel_name[,vni][,mac] */
    return endpoint_.to_string() + "@" + tunnel_name_ + ',' + vni_;
}

void EniAclRule::setKey(EniInfo& eni)
{
    name_ = string(DashEniFwd::TABLE) + ":" + eni.toKey();
    if (type_ == rule_type_t::TUNNEL_TERM)
    {
        name_ += "_TERM";
    }
}

update_type_t EniAclRule::processUpdate(EniInfo& eni)
{
    SWSS_LOG_ENTER();
    auto& ctx = eni.getCtx();
    IpAddress primary_endp;
    dpu_type_t primary_type = LOCAL;
    update_type_t update_type = PRIMARY_UPDATE;
    std::string primary_id;

    if (type_ == rule_type_t::TUNNEL_TERM)
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
        SWSS_LOG_ERROR("No primary id %s in DPU Table", primary_id.c_str());
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
        ctx->deleteAclRule(key);
        setState(rule_state_t::UNINSTALLED);
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

    if (type_ == rule_type_t::TUNNEL_TERM)
    {
        fv_.push_back({MATCH_TUNNEL_TERM, "true"});
    }
    
    ctx->createAclRule(key, fv_);
    setState(INSTALLED);
}

string EniAclRule::getMacMatchDirection(EniInfo& eni)
{
    return MATCH_INNER_DST_MAC;
}

void EniAclRule::destroy(EniInfo& eni)
{
    if (state_ == rule_state_t::INSTALLED)
    {
        auto key = getKey();
        auto& ctx = eni.getCtx();
        ctx->deleteAclRule(key);
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
    auto itr_ep_list = updates.find(DashEniFwd::VDPU_IDS);
    auto itr_primary_id = updates.find(DashEniFwd::PRIMARY);

    /* Validation Checks */
    if (itr_ep_list == updates.end() || itr_primary_id == updates.end())
    {
        SWSS_LOG_ERROR("Invalid DASH_ENI_FORWARD_TABLE request: No endpoint/primary");
        return false;
    }

    ep_list_ = db_request.getAttrStringList(DashEniFwd::VDPU_IDS);
    primary_id_ = db_request.getAttrString(DashEniFwd::PRIMARY);

    std::string local_id;
    bool tunn_term_allow = findLocalEp(local_id);

    /* Create Rules */
    rule_container_.emplace(piecewise_construct,
                forward_as_tuple(rule_type_t::NO_TUNNEL_TERM),
                forward_as_tuple(rule_type_t::NO_TUNNEL_TERM, *this));

    if (tunn_term_allow)
    {
        /* Create rule for tunnel termination if required */
        rule_container_.emplace(piecewise_construct,
                    forward_as_tuple(rule_type_t::TUNNEL_TERM),
                    forward_as_tuple(rule_type_t::TUNNEL_TERM, *this));
    }

    fireAllRules();
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
    auto itr_primary_id = updates.find(DashEniFwd::PRIMARY);

    /* Validation Checks */
    if (itr_primary_id == updates.end())
    {
        throw logic_error("Invalid DASH_ENI_FORWARD_TABLE update: No primary idx");
    }

    if (getPrimaryId() == db_request.getAttrString(DashEniFwd::PRIMARY))
    {
        /* No update in the primary id, return true */
        return true;
    }

    /* Update local primary id and fire the rules */
    primary_id_ = db_request.getAttrString(DashEniFwd::PRIMARY);
    fireAllRules();

    return true;
}

bool EniInfo::findLocalEp(std::string& local_endpoint) const
{
    /* Check if atleast one of the endpoints is local */
    bool found = false;
    for (auto idx : ep_list_)
    {   
        dpu_type_t val = dpu_type_t::CLUSTER;
        if (ctx->dpu_info.getType(idx, val) && val == dpu_type_t::LOCAL)
        {
            if (!found)
            {
                found = true;
                local_endpoint = idx;
            }
            else
            {
                SWSS_LOG_WARN("Multiple Local Endpoints for the ENI %s found, proceeding with %s",
                                mac_.to_string().c_str(), local_endpoint.c_str());
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
