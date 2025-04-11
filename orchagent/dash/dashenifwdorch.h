#pragma once

#include <set>
#include <map>
#include "producerstatetable.h"
#include "orchagent/orch.h"
#include "orchagent/portsorch.h"
#include "orchagent/aclorch.h"
#include "orchagent/neighorch.h"
#include "orchagent/vnetorch.h"
#include "orchagent/observer.h"
#include "orchagent/request_parser.h"
#include <exception>
#include <functional>

// TODO: remove after the schema is finalized and added to swss-common
#define CFG_VIP_TABLE_TMP          "VIP_TABLE"

#define ENI_REDIRECT_TABLE_TYPE    "ENI_REDIRECT"
#define ENI_REDIRECT_TABLE         "ENI"
#define ENI_FWD_VDPU_IDS           "vdpu_ids"
#define ENI_FWD_PRIMARY            "primary_vdpu"
#define ENI_FWD_OUT_VNI            "outbound_vni"
#define ENI_FWD_OUT_MAC_LOOKUP     "outbound_eni_mac_lookup"

#define DPU_TYPE     "type"
#define DPU_STATE    "state"
#define DPU_PA_V4    "pa_ipv4"
#define DPU_PA_V6    "pa_ipv6"
#define DPU_NPU_V4   "npu_ipv4"
#define DPU_NPU_V6   "npu_ipv6"

#define DPU_LOCAL      "local"
#define OUT_MAC_DIR    "dst"



const request_description_t eni_dash_fwd_desc = {
    { REQ_T_STRING, REQ_T_MAC_ADDRESS }, // VNET_NAME, ENI_ID
    {
        { ENI_FWD_VDPU_IDS,               REQ_T_UINT_LIST }, // DPU ID's
        { ENI_FWD_PRIMARY,                REQ_T_UINT },
        { ENI_FWD_OUT_VNI,                REQ_T_UINT },
        { ENI_FWD_OUT_MAC_LOOKUP,         REQ_T_STRING },
    },
    {  }
};

const request_description_t dpu_table_desc = {
    { REQ_T_UINT }, // DPU_ID
    {
        { DPU_TYPE,              REQ_T_STRING },
        { DPU_STATE,             REQ_T_STRING },
        { DPU_PA_V4,             REQ_T_IP },
        { DPU_PA_V6,             REQ_T_IP },
        { DPU_NPU_V4,            REQ_T_IP },
        { DPU_NPU_V6,            REQ_T_IP },
    },
    { DPU_TYPE, DPU_PA_V4, DPU_NPU_V4 }
};

typedef enum
{
    LOCAL,
    CLUSTER,
    EXTERNAL
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
    INBOUND = 0,
    OUTBOUND,
    INBOUND_TERM,
    OUTBOUND_TERM
} rule_type_t;


class DpuRegistry;
class EniNH; 
class LocalEniNH;
class RemoteEniNH;
class EniAclRule;
class EniInfo;
class EniFwdCtxBase;
class EniFwdCtx;


class DashEniFwdOrch : public Orch2, public Observer
{
public:
    struct EniFwdRequest : public Request
    {
        EniFwdRequest() : Request(eni_dash_fwd_desc, ':') {}
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
    void initAclTableCfg();
    void initLocalEndpoints();
    void handleNeighUpdate(const NeighborUpdate& update);
    void handleEniDpuMapping(uint64_t id, MacAddress mac, bool add = true);

    /* multimap because Multiple ENIs can be mapped to the same DPU */
    std::multimap<uint64_t, swss::MacAddress> dpu_eni_map_;
    /* Local Endpoint -> DPU mapping */
    std::map<swss::IpAddress, uint64_t> neigh_dpu_map_;
    std::map<swss::MacAddress, EniInfo> eni_container_;

    bool ctx_initialized_ = false;
    shared_ptr<EniFwdCtxBase> ctx;
    unique_ptr<swss::ProducerStateTable> acl_table_;
    unique_ptr<swss::ProducerStateTable> acl_table_type_;
    NeighOrch* neighorch_;
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
        DpuRequest() : Request(dpu_table_desc, '|' ) { }
    };

    void populate(Table*);
    std::vector<uint64_t> getIds();
    bool getType(uint64_t id, dpu_type_t& val);
    bool getPaV4(uint64_t id, swss::IpAddress& val);
    bool getNpuV4(uint64_t id, swss::IpAddress& val);

private:
    void processDpuTable(const KeyOpFieldsValuesTuple& );

    std::vector<uint64_t> dpus_ids_;
    DpuRequest dpu_request_;
    map<uint64_t, DpuData> dpus_;
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
    virtual void destroy(EniInfo& eni) = 0;
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
    void destroy(EniInfo& eni) {}
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
    void destroy(EniInfo& eni) override;
    string getRedirectVal() override;

private:
    string tunnel_name_;
};


class EniAclRule
{
public:
    static const int BASE_PRIORITY;
    static const std::vector<std::string> RULE_NAMES;

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
    bool findLocalEp(uint64_t&) const;
    swss::MacAddress getMac() const { return mac_; } // Can only be set during object creation
    std::vector<uint64_t> getEpList() { return ep_list_; }
    uint64_t getPrimaryId() const { return primary_id_; }
    uint64_t getOutVni() const { return outbound_vni_; }
    std::string getOutMacLookup() const { return outbound_mac_lookup_; }
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
    std::vector<uint64_t> ep_list_;
    uint64_t primary_id_;
    uint64_t outbound_vni_;
    std::string outbound_mac_lookup_;
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
    bool handleTunnelNH(const std::string&, swss::IpAddress, bool);
    swss::IpPrefix getVip();

    virtual void initialize() = 0;
    /* API's that call other orchagents */
    virtual std::map<std::string, Port>& getAllPorts() = 0;
    virtual bool isNeighborResolved(const NextHopKey&) = 0;
    virtual void resolveNeighbor(const NeighborEntry &) = 0;
    virtual string getRouterIntfsAlias(const IpAddress &, const string & = "") = 0;
    virtual bool findVnetVni(const std::string&, uint64_t& ) = 0;
    virtual bool findVnetTunnel(const std::string&, string&) = 0;
    virtual sai_object_id_t createNextHopTunnel(string, IpAddress) = 0;
    virtual bool removeNextHopTunnel(string, IpAddress) = 0;

    DpuRegistry dpu_info;
    unique_ptr<swss::ProducerStateTable> rule_table;
protected:
    std::set<std::string> findInternalPorts();

    /* RemoteNh Key -> {ref_count, sai oid} */
    std::map<std::string, std::pair<uint32_t, sai_object_id_t>> remote_nh_map_;
    /* Mapping between DPU Nbr and Alias */
    std::map<swss::IpAddress, std::string> nh_alias_map_;

    unique_ptr<swss::Table> port_tbl_;
    unique_ptr<swss::Table> vip_tbl_;
    unique_ptr<swss::Table> dpu_tbl_;

    /* Only one vip is expected per T1 */
    swss::IpPrefix vip; 
    bool vip_inferred_;
};


/*
    Wrapper on API's that are throwable
*/
template <typename C, typename R, typename... Args>
R safetyWrapper(C* ptr, R (C::*func)(Args...), R defaultValue, Args&&... args)
{
    SWSS_LOG_ENTER();
    try
    {
        return (ptr->*func)(std::forward<Args>(args)...);
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Exception thrown.. %s", e.what());
        return defaultValue;
    }
}


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
    virtual sai_object_id_t createNextHopTunnel(string, IpAddress) override;
    virtual bool removeNextHopTunnel(string, IpAddress) override;

private:
    PortsOrch* portsorch_;
    NeighOrch* neighorch_;
    IntfsOrch* intfsorch_;
    VNetOrch* vnetorch_;
    VxlanTunnelOrch* vxlanorch_;
};
