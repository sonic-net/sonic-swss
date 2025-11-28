#pragma once

#include <set>
#include <map>
#include "producerstatetable.h"
#include "orch.h"
#include "portsorch.h"
#include "aclorch.h"
#include "neighorch.h"
#include "vnetorch.h"
#include "observer.h"
#include "request_parser.h"
#include <exception>
#include <functional>

typedef enum
{
    LOCAL,
    CLUSTER
} dpu_type_t;

typedef enum
{
    RESOLVED,
    UNRESOLVED
} endpoint_status_t;

typedef enum
{
    FAILED,
    PENDING,
    INSTALLED,
    UNINSTALLED
} rule_state_t;

typedef enum
{
    INVALID,
    IDEMPOTENT,
    CREATE,
    PRIMARY_UPDATE /* Either NH update or primary endp change */
} update_type_t;

typedef enum
{
    NO_TUNNEL_TERM = 0,
    TUNNEL_TERM
} rule_type_t;


class DpuRegistry;
class EniNH; 
class LocalEniNH;
class RemoteEniNH;
class EniAclRule;
class EniInfo;
class EniFwdCtxBase;
class EniFwdCtx;

namespace DashEniFwd
{
    /* TABLES; Until finalized and added to sonic-swss-common */
    static constexpr const char* DPU_TABLE          = "DPU";
    static constexpr const char* REMOTE_DPU_TABLE   = "REMOTE_DPU";
    static constexpr const char* VDPU_TABLE         = "VDPU";
    static constexpr const char* VIP_TABLE          = "VIP_TABLE";

    /* ENI Registry Fields */
    static constexpr const char* TABLE_TYPE         = "ENI_REDIRECT";
    static constexpr const char* TABLE              = "ENI";
    static constexpr const char* VDPU_IDS           = "vdpu_ids";
    static constexpr const char* PRIMARY            = "primary_vdpu";

    /* DPU Registry Fields */
    static constexpr const char* STATE              = "state";
    static constexpr const char* PA_V4              = "pa_ipv4";
    static constexpr const char* PA_V6              = "pa_ipv6";
    static constexpr const char* NPU_V4             = "npu_ipv4";
    static constexpr const char* NPU_V6             = "npu_ipv6";
    static constexpr const char* DPU_IDS            = "main_dpu_ids";
};

const request_description_t eni_dash_fwd_desc = {
    { REQ_T_STRING, REQ_T_MAC_ADDRESS }, // VNET_NAME, ENI_ID
    {
        { DashEniFwd::VDPU_IDS,               REQ_T_STRING_LIST }, // VDPU ID's
        { DashEniFwd::PRIMARY,                REQ_T_STRING },
    },
    { DashEniFwd::PRIMARY }
};

class DashEniFwdOrch : public Orch2, public Observer
{
public:
    struct EniFwdRequest : public Request
    {
        EniFwdRequest() : Request(eni_dash_fwd_desc, ':', true) {}
    };
    
    DashEniFwdOrch(swss::DBConnector*, swss::DBConnector*, const std::string&, NeighOrch* neigh_orch_);
    ~DashEniFwdOrch();

    /* Refresh the ENIs based on NextHop status */
    void update(SubjectType, void *) override;

protected:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);
    EniFwdRequest request_;

private:
    void lazyInit();
    void initLocalEndpoints();
    void handleNeighUpdate(const NeighborUpdate& update);
    void handleEniDpuMapping(const std::string& id, MacAddress mac, bool add = true);

    /* multimap because Multiple ENIs can be mapped to the same DPU */
    std::multimap<std::string, swss::MacAddress> dpu_eni_map_;
    /* Local Endpoint -> DPU mapping */
    std::map<swss::IpAddress, std::string> neigh_dpu_map_;
    std::map<swss::MacAddress, EniInfo> eni_container_;

    bool ctx_initialized_ = false;
    shared_ptr<EniFwdCtxBase> ctx;
    NeighOrch* neighorch_;
};


const request_description_t dpu_table_desc = {
    { REQ_T_STRING },
    {
        { DashEniFwd::STATE,    REQ_T_STRING },
        { DashEniFwd::PA_V4,    REQ_T_IP },
        { DashEniFwd::PA_V6,    REQ_T_IP },
    },
    { DashEniFwd::STATE, DashEniFwd::PA_V4 }
};

const request_description_t remote_dpu_table_desc = {
    { REQ_T_STRING },
    {
        { DashEniFwd::PA_V4,    REQ_T_IP },
        { DashEniFwd::PA_V6,    REQ_T_IP },
        { DashEniFwd::NPU_V4,   REQ_T_IP },
        { DashEniFwd::NPU_V6,   REQ_T_IP },
    },
    { DashEniFwd::PA_V4, DashEniFwd::NPU_V4 }
};

const request_description_t vdpu_table_desc = {
    { REQ_T_STRING },
    {
        { DashEniFwd::DPU_IDS,   REQ_T_STRING_LIST },
    },
    { DashEniFwd::DPU_IDS }
};

class DpuRegistry
{
public:
    struct DpuData
    {
        dpu_type_t type;
        swss::IpAddress pa_v4;
        swss::IpAddress npu_v4;
    };

    struct DpuRequest : public Request
    {
        DpuRequest() : Request(dpu_table_desc, '|', true) {}
    };
    struct RemoteDpuRequest : public Request
    {
        RemoteDpuRequest() : Request(remote_dpu_table_desc, '|', true) {}
    };
    struct VdpuRequest : public Request
    {
        VdpuRequest() : Request(vdpu_table_desc, '|', true) {}
    };

    void populate(const swss::DBConnector*);
    std::vector<std::string> getIds();

    bool getDpuId(const std::string& vdpu_id, std::string& dpu_id);
    bool getType(const std::string& vdpu_id, dpu_type_t& val);
    bool getPaV4(const std::string& vdpu_id, swss::IpAddress& val);
    bool getNpuV4(const std::string& vdpu_id, swss::IpAddress& val);

private:
    void processDpuTable(const swss::DBConnector*);
    void processRemoteDpuTable(const swss::DBConnector*);
    void processVdpuTable(const swss::DBConnector*);

    DpuRequest dpu_request_;
    RemoteDpuRequest remote_dpu_request_;
    VdpuRequest vdpu_request_;
    // DPU -> DpuData
    unordered_map<std::string, DpuData> dpus_name_map_;
    // VDPU Name -> [DPU2, DPU3, ...]
    unordered_map<std::string, vector<std::string>> vdpus_map_; 
};


class EniNH
{
public:
    static std::unique_ptr<EniNH> createNextHop(dpu_type_t, const swss::IpAddress&);

    EniNH(const swss::IpAddress& ip) : endpoint_(ip) {}
    void setStatus(endpoint_status_t status) {status_ = status;}
    void setType(dpu_type_t type) {type_ = type;}
    endpoint_status_t getStatus() {return status_;}
    dpu_type_t getType() {return type_;}
    swss::IpAddress getEp() {return endpoint_;}

    virtual void resolve(EniInfo& eni) = 0;
    virtual void destroy(EniInfo& eni) {};
    virtual string getRedirectVal() = 0;

protected:
    endpoint_status_t status_;
    dpu_type_t type_;
    swss::IpAddress endpoint_;
};


class LocalEniNH : public EniNH
{
public:
    LocalEniNH(const swss::IpAddress& ip) : EniNH(ip)
    {
        setStatus(endpoint_status_t::UNRESOLVED);
        setType(dpu_type_t::LOCAL);
    }
    void resolve(EniInfo& eni) override;
    string getRedirectVal() override;
};


class RemoteEniNH : public EniNH
{
public: 
    RemoteEniNH(const swss::IpAddress& ip) : EniNH(ip) 
    {
        /* No BFD monitoring for Remote NH yet */
        setStatus(endpoint_status_t::UNRESOLVED);
        setType(dpu_type_t::CLUSTER);
    }
    void resolve(EniInfo& eni) override;
    string getRedirectVal() override;

private:
    string tunnel_name_;
    string vni_;
};


class EniAclRule
{
public:
    static const int BASE_PRIORITY;

    EniAclRule(rule_type_t type, EniInfo& eni) :
        type_(type),
        state_(rule_state_t::PENDING) { setKey(eni); }

    void destroy(EniInfo&);
    void fire(EniInfo&);

    update_type_t processUpdate(EniInfo& eni);
    std::string getKey() {return name_; }
    string getMacMatchDirection(EniInfo& eni);
    void setState(rule_state_t state);

private:
    void setKey(EniInfo&);
    std::unique_ptr<EniNH> nh_ = nullptr;
    std::string name_;
    rule_type_t type_;
    rule_state_t state_;
};


class EniInfo
{
public:
    friend class DashEniFwdOrch; /* Only orch is expected to call create/update/fire */

    EniInfo(const std::string&, const std::string&, const shared_ptr<EniFwdCtxBase>&);
    EniInfo(const EniInfo&) = delete;
    EniInfo& operator=(const EniInfo&) = delete;
    EniInfo(EniInfo&&) = delete;
    EniInfo& operator=(EniInfo&&) = delete;
    
    string toKey() const;
    std::shared_ptr<EniFwdCtxBase>& getCtx() {return ctx;}
    bool findLocalEp(std::string&) const;
    swss::MacAddress getMac() const { return mac_; } // Can only be set during object creation
    std::vector<std::string> getEpList() { return ep_list_; }
    std::string getPrimaryId() const { return primary_id_; }
    std::string getVnet() const { return vnet_name_; }

protected:
    void formatMac();
    void fireRule(rule_type_t);
    void fireAllRules();
    bool create(const Request&);
    bool destroy(const Request&);
    bool update(const Request& );
    bool update(const NeighborUpdate&);

    std::shared_ptr<EniFwdCtxBase> ctx;
    std::map<rule_type_t, EniAclRule> rule_container_;
    std::vector<std::string> ep_list_;
    std::string primary_id_;
    std::string vnet_name_;
    swss::MacAddress mac_;
    std::string mac_key_; // Formatted MAC key
};


/* 
    Collection of API's used across DashEniFwdOrch
*/
class EniFwdCtxBase
{
public:
    EniFwdCtxBase(DBConnector* cfgDb, DBConnector* applDb);
    void populateDpuRegistry();
    std::vector<std::string> getBindPoints();
    std::string getNbrAlias(const swss::IpAddress& ip);
    swss::IpPrefix getVip();

    void createAclRule(const std::string&, const std::vector<FieldValueTuple>&);
    void deleteAclRule(const std::string&);

    virtual void initialize() = 0;
    /* API's that call other orchagents */
    virtual std::map<std::string, Port>& getAllPorts() = 0;
    virtual bool isNeighborResolved(const NextHopKey&) = 0;
    virtual void resolveNeighbor(const NeighborEntry &) = 0;
    virtual string getRouterIntfsAlias(const IpAddress &, const string & = "") = 0;
    virtual bool findVnetVni(const std::string&, uint64_t& ) = 0;
    virtual bool findVnetTunnel(const std::string&, string&) = 0;

    DpuRegistry dpu_info;

protected:
    std::set<std::string> findInternalPorts();
    void addAclTable();
    void deleteAclTable();
    /* Reference counting for ACL rules */
    uint32_t acl_rule_count_ = 0;

    /* Mapping between DPU Nbr and Alias */
    std::map<swss::IpAddress, std::string> nh_alias_map_;

    unique_ptr<swss::Table> port_tbl_;
    unique_ptr<swss::Table> vip_tbl_;
    unique_ptr<swss::DBConnector> cfg_db_;
    unique_ptr<swss::ProducerStateTable> rule_table_;
    unique_ptr<swss::ProducerStateTable> acl_table_;
    unique_ptr<swss::ProducerStateTable> acl_table_type_;

    /* Only one vip is expected per T1 cluster */
    swss::IpPrefix vip; 
    bool vip_inferred_;
};


/* 
    Implements API's to access other orchagents
*/
class EniFwdCtx : public EniFwdCtxBase
{
public:
    using EniFwdCtxBase::EniFwdCtxBase;

    /* Setup pointers to other orchagents */
    void initialize() override;
    bool isNeighborResolved(const NextHopKey&) override;
    void resolveNeighbor(const NeighborEntry&) override;
    std::string getRouterIntfsAlias(const IpAddress &, const string & = "") override;
    bool findVnetVni(const std::string&, uint64_t&) override;
    bool findVnetTunnel(const std::string&, string&) override;
    std::map<std::string, Port>& getAllPorts() override;

private:
    PortsOrch* portsorch_;
    NeighOrch* neighorch_;
    IntfsOrch* intfsorch_;
    VNetOrch* vnetorch_;
    VxlanTunnelOrch* vxlanorch_;
};
