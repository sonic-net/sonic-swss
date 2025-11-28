#include <memory>
#include <numeric>
#include "dashenifwdorch.h"
#include "directory.h"

extern Directory<Orch*>      gDirectory;

using namespace swss;
using namespace std;

DashEniFwdOrch::DashEniFwdOrch(DBConnector* cfgDb, DBConnector* applDb, const std::string& tableName, NeighOrch* neighOrch)
    : Orch2(applDb, tableName, request_), neighorch_(neighOrch)
{
    SWSS_LOG_ENTER();
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
    /*
        Refresh ENI's that are hosted on the DPU with the corresponding Neighboo
    */
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

void DashEniFwdOrch::initLocalEndpoints()
{
    auto ids = ctx->dpu_info.getIds();
    dpu_type_t primary_type = CLUSTER;
    IpAddress local_endp;
    for (auto id : ids)
    {
        if(ctx->dpu_info.getType(id, primary_type) && primary_type == dpu_type_t::LOCAL)
        {
            if(ctx->dpu_info.getPaV4(id, local_endp))
            {
                neigh_dpu_map_.insert(make_pair(local_endp, id));
                SWSS_LOG_NOTICE("Local DPU endpoint detected %s", local_endp.to_string().c_str());

                /* Try to resovle the neighbor */
                auto alias = ctx->getNbrAlias(local_endp);
                NextHopKey nh(local_endp, alias);

                if (ctx->isNeighborResolved(nh))
                {
                    SWSS_LOG_INFO("Neighbor already populated for local endpoint %s", local_endp.to_string().c_str());
                }
                ctx->resolveNeighbor(nh);
            }
        }
    }
}

void DashEniFwdOrch::handleEniDpuMapping(const std::string& id, MacAddress mac, bool add)
{
    /* Make sure id is local */
    dpu_type_t primary_type = CLUSTER;
    if(ctx->dpu_info.getType(id, primary_type) && primary_type == dpu_type_t::LOCAL)
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
    */
    ctx->initialize();
    ctx->populateDpuRegistry();
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
        std::string local_ep;
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
        std::string local_ep;
        if (eni_itr->second.findLocalEp(local_ep))
        {
            handleEniDpuMapping(local_ep, eni_id, false);
        }
    }
    eni_container_.erase(eni_id);
    return true;
}


void DpuRegistry::populate(const DBConnector* cfg_db)
{
    /* 
        Read DPU, VDPU, and Remote DPU tables, they are expected to be populated by the time HA is ready 
    */
    SWSS_LOG_ENTER();
    processDpuTable(cfg_db);
    processRemoteDpuTable(cfg_db);
    processVdpuTable(cfg_db);
}

void DpuRegistry::processDpuTable(const DBConnector* cfg_db)
{
    Table dpuTable(cfg_db, DashEniFwd::DPU_TABLE);
    std::vector<std::string> keys;
    dpuTable.getKeys(keys);
    for (auto key : keys)
    {
        try
        {
            std::vector<FieldValueTuple> values;
            dpuTable.get(key, values);

            KeyOpFieldsValuesTuple kvo = {
                key, SET_COMMAND, values
            };
            
            dpu_request_.clear();
            dpu_request_.parse(kvo);
            string key = dpu_request_.getKeyString(0);
            // Check if STATE is present and if present and value is 'down', skip this DPU
            auto updates = dpu_request_.getAttrFieldNames();
            auto itr_state = updates.find(DashEniFwd::STATE);
            if (itr_state != updates.end())
            {
                auto state_val = dpu_request_.getAttrString(DashEniFwd::STATE);
                if (state_val == "down")
                {
                    SWSS_LOG_INFO("Skipping LOCAL DPU %s as its state is down", key.c_str());
                    continue;
                }
            }

            DpuData data;
            data.type = dpu_type_t::LOCAL;
            data.pa_v4 = dpu_request_.getAttrIP(DashEniFwd::PA_V4);
            dpus_name_map_.insert({key, data});
            
            SWSS_LOG_INFO("LOCAL DPU %s found, PA_V4: %s", key.c_str(), data.pa_v4.to_string().c_str());
        }
        catch(exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse key:%s in the %s", key.c_str(), DashEniFwd::DPU_TABLE);
        }
    }
}

void DpuRegistry::processRemoteDpuTable(const DBConnector* cfg_db)
{
    Table remoteDpuTable(cfg_db, DashEniFwd::REMOTE_DPU_TABLE);
    std::vector<std::string> keys;
    remoteDpuTable.getKeys(keys);
    for (auto key : keys)
    {
        try
        {
            std::vector<FieldValueTuple> values;
            remoteDpuTable.get(key, values);

            KeyOpFieldsValuesTuple kvo = {
                key, SET_COMMAND, values
            };
            
            remote_dpu_request_.clear();
            remote_dpu_request_.parse(kvo);
            string key = remote_dpu_request_.getKeyString(0);

            DpuData data;
            data.type = dpu_type_t::CLUSTER;
            data.pa_v4 = remote_dpu_request_.getAttrIP(DashEniFwd::PA_V4);
            data.npu_v4 = remote_dpu_request_.getAttrIP(DashEniFwd::NPU_V4);
            dpus_name_map_.insert({key, data});
        
            SWSS_LOG_INFO("Remote DPU %s found, PA_V4: %s, NPU_V4: %s", 
                key.c_str(), 
                data.pa_v4.to_string().c_str(), 
                data.npu_v4.to_string().c_str()
            );
        }
        catch(exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse key:%s in the %s", key.c_str(), DashEniFwd::REMOTE_DPU_TABLE);
        }
    }
}

void DpuRegistry::processVdpuTable(const DBConnector* cfg_db)
{
    Table vdpuTable(cfg_db, DashEniFwd::VDPU_TABLE);
    std::vector<std::string> keys;
    vdpuTable.getKeys(keys);
    for (auto key : keys)
    {
        try
        {
            std::vector<FieldValueTuple> values;
            vdpuTable.get(key, values);

            KeyOpFieldsValuesTuple kvo = {
                key, SET_COMMAND, values
            };
            
            vdpu_request_.clear();
            vdpu_request_.parse(kvo);
            string key = vdpu_request_.getKeyString(0);
            vector<string> dpu_ids = vdpu_request_.getAttrStringList(DashEniFwd::DPU_IDS);
            for (auto dpu_id : dpu_ids)
            {
                /* This method is expected to be called after the DPU/REMOTE_DPU table is populated */
                if (dpus_name_map_.find(dpu_id) != dpus_name_map_.end())
                {
                    vdpus_map_[key].push_back(dpu_id);
                    SWSS_LOG_INFO("DPU: %s belongs to VDPU %s", dpu_id.c_str(), key.c_str());
                }
                else 
                {
                    SWSS_LOG_WARN("Invalid DPU ID: %s, not found in DPU/REMOTE_DPU table", dpu_id.c_str());
                }
            }
        }
        catch(exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse key:%s in the %s", key.c_str(), DashEniFwd::REMOTE_DPU_TABLE);
        }
    }
}

std::vector<std::string> DpuRegistry::getIds()
{
    std::vector<std::string> ids;
    for (auto itr = vdpus_map_.begin(); itr != vdpus_map_.end(); itr++)
    {
        ids.push_back(itr->first);
    }
    return ids;
}

bool DpuRegistry::getDpuId(const std::string& vdpu_id, std::string& dpu_id)
{
    dpu_id.clear();
    auto itr = vdpus_map_.find(vdpu_id);
    if (itr == vdpus_map_.end() || itr->second.empty()) return false;
    dpu_id = itr->second[0];
    return true;
}

bool DpuRegistry::getType(const std::string& vdpu_id, dpu_type_t& val)
{
    std::string id;
    if (!getDpuId(vdpu_id, id)) return false;
    auto itr = dpus_name_map_.find(id);
    if (itr == dpus_name_map_.end()) return false;
    val = itr->second.type;
    return true;
}

bool DpuRegistry::getPaV4(const std::string& vdpu_id, swss::IpAddress& val)
{
    std::string id;
    if (!getDpuId(vdpu_id, id)) return false;
    auto itr = dpus_name_map_.find(id);
    if (itr == dpus_name_map_.end()) return false;
    val = itr->second.pa_v4;
    return true;
}

bool DpuRegistry::getNpuV4(const std::string& vdpu_id, swss::IpAddress& val)
{
    std::string id;
    if (!getDpuId(vdpu_id, id)) return false;
    auto itr = dpus_name_map_.find(id);
    if (itr == dpus_name_map_.end()) return false;
    val = itr->second.npu_v4;
    return true;
}

EniFwdCtxBase::EniFwdCtxBase(DBConnector* cfgDb, DBConnector* applDb)
{
    cfg_db_ = make_unique<DBConnector>(*cfgDb);
    port_tbl_ = make_unique<Table>(cfgDb, CFG_PORT_TABLE_NAME);
    vip_tbl_ = make_unique<Table>(cfgDb, DashEniFwd::VIP_TABLE);
    rule_table_ = make_unique<ProducerStateTable>(applDb, APP_ACL_RULE_TABLE_NAME);
    acl_table_type_ = make_unique<ProducerStateTable>(applDb, APP_ACL_TABLE_TYPE_TABLE_NAME);
    acl_table_ = make_unique<ProducerStateTable>(applDb, APP_ACL_TABLE_TABLE_NAME);
    vip_inferred_ = false;
}

void EniFwdCtxBase::populateDpuRegistry() 
{
    dpu_info.populate(cfg_db_.get());
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

bool EniFwdCtx::isNeighborResolved(const NextHopKey& nh) 
{
    return neighorch_->isNeighborResolved(nh);
}

void EniFwdCtx::resolveNeighbor(const NeighborEntry& nh) 
{
    /*  Neighorch already has the logic to handle the duplicate requests */
    neighorch_->resolveNeighbor(nh);
}

string EniFwdCtx::getRouterIntfsAlias(const IpAddress &ip, const string &vrf_name)
{
    return intfsorch_->getRouterIntfsAlias(ip, vrf_name);
}

bool EniFwdCtx::findVnetVni(const string& vnet_name, uint64_t& vni)
{
    if (vnetorch_->isVnetExists(vnet_name))
    {
        vni = vnetorch_->getTypePtr<VNetObject>(vnet_name)->getVni();
        return true;
    }
    return false;
}

bool EniFwdCtx::findVnetTunnel(const string& vnet_name, string& tunnel) 
{
    if (vnetorch_->isVnetExists(vnet_name))
    {
        tunnel = vnetorch_->getTunnelName(vnet_name);
        return true;
    }
    return false;
}

std::map<string, Port>& EniFwdCtx::getAllPorts() 
{
    return portsorch_->getAllPorts();
}

void EniFwdCtxBase::createAclRule(const std::string& rule, const std::vector<FieldValueTuple>& fv)
{
    if (acl_rule_count_ == 0)
    {
        addAclTable();
    }
    acl_rule_count_++;
    SWSS_LOG_INFO("Creating ACL rule: %s, ENI Forwarding rules count: %u", rule.c_str(), acl_rule_count_);
    rule_table_->set(rule, fv);
}

void EniFwdCtxBase::deleteAclRule(const std::string& rule)
{
    rule_table_->del(rule);
    if (acl_rule_count_ > 0)
    {
        acl_rule_count_--;
        SWSS_LOG_INFO("Deleted ACL rule: %s, ENI Forwarding rule count: %u", rule.c_str(), acl_rule_count_);
        if (acl_rule_count_ == 0)
        {
            deleteAclTable();
        }
    }
    else
    {
        SWSS_LOG_ERROR("Attempted to delete ACL rule %s but rule count is already 0", rule.c_str());
    }
}

void EniFwdCtxBase::addAclTable()
{
    vector<string> match_list = {
                                  MATCH_DST_IP,
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

    acl_table_type_->set(DashEniFwd::TABLE_TYPE, fv_);

    auto ports = getBindPoints();
    std::string ports_str;

    if (!ports.empty())
    {
        ports_str = std::accumulate(std::next(ports.begin()), ports.end(), ports[0], concat);
    }

    /* Write ACL Table */
    vector<FieldValueTuple> table_fv_ = {
        { ACL_TABLE_DESCRIPTION, "Contains Rule for DASH ENI Based Forwarding"},
        { ACL_TABLE_TYPE, DashEniFwd::TABLE_TYPE },
        { ACL_TABLE_STAGE, STAGE_INGRESS },
        { ACL_TABLE_PORTS, ports_str }
    };

    acl_table_->set(DashEniFwd::TABLE, table_fv_);
}

void EniFwdCtxBase::deleteAclTable()
{
    acl_table_->del(DashEniFwd::TABLE);
    acl_table_type_->del(DashEniFwd::TABLE_TYPE);
}
