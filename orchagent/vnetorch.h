#ifndef __VNETORCH_H
#define __VNETORCH_H

#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <bitset>
#include <tuple>

#include "aclorch.h"
#include "request_parser.h"
#include "ipaddresses.h"
#include "producerstatetable.h"
#include "observer.h"
#include "nexthopgroupkey.h"
#include "bfdorch.h"
#include "tunneltermhelper.h"

#define VNET_BITMAP_SIZE 32
#define VNET_TUNNEL_SIZE 40960
#define VNET_ROUTE_FULL_MASK_OFFSET_MAX 3000
#define VNET_NEIGHBOR_MAX 0xffff
#define VXLAN_ENCAP_TTL 128
#define VNET_BITMAP_RIF_MTU 9100

#define VNET_MONITORING_TYPE_CUSTOM "custom"
#define VNET_MONITORING_TYPE_CUSTOM_BFD "custom_bfd"

extern sai_object_id_t gVirtualRouterId;


typedef enum
{
    MONITOR_SESSION_STATE_UNKNOWN,
    MONITOR_SESSION_STATE_UP,
    MONITOR_SESSION_STATE_DOWN,
} monitor_session_state_t;

const request_description_t vnet_request_description = {
    { REQ_T_STRING },
    {
        { "src_mac",            REQ_T_MAC_ADDRESS },
        { "vxlan_tunnel",       REQ_T_STRING },
        { "vni",                REQ_T_UINT },
        { "peer_list",          REQ_T_SET },
        { "guid",               REQ_T_STRING },
        { "scope",              REQ_T_STRING },
        { "advertise_prefix",   REQ_T_BOOL},
        { "overlay_dmac",       REQ_T_MAC_ADDRESS},

    },
    { "vxlan_tunnel", "vni" } // mandatory attributes
};

enum class VNET_EXEC
{
    VNET_EXEC_VRF,
    VNET_EXEC_BRIDGE,
    VNET_EXEC_INVALID
};

enum class VR_TYPE
{
    ING_VR_VALID,
    EGR_VR_VALID,
    VR_INVALID
};

struct VNetInfo
{
    string tunnel;
    uint32_t vni;
    set<string> peers;
    string scope;
    bool advertise_prefix;
    swss::MacAddress overlay_dmac;
};

typedef map<VR_TYPE, sai_object_id_t> vrid_list_t;
extern std::vector<VR_TYPE> vr_cntxt;

class VNetRequest : public Request
{
public:
    VNetRequest() : Request(vnet_request_description, ':') { }
};

struct NextHopGroupInfo
{
    sai_object_id_t                         next_hop_group_id;      // next hop group id (null for single nexthop)
    int                                     ref_count;              // reference count
    std::map<NextHopKey, sai_object_id_t>   active_members;         // active nexthops and nexthop group member id (null for single nexthop)
    std::set<IpPrefix>                      tunnel_routes;
};

class VNetObject
{
public:
    VNetObject(const VNetInfo& vnetInfo) :
               tunnel_(vnetInfo.tunnel),
               peer_list_(vnetInfo.peers),
               vni_(vnetInfo.vni),
               scope_(vnetInfo.scope),
               advertise_prefix_(vnetInfo.advertise_prefix),
               overlay_dmac_(vnetInfo.overlay_dmac)
               { }

    virtual bool updateObj(vector<sai_attribute_t>&) = 0;

    void setPeerList(set<string>& p_list)
    {
        peer_list_ = p_list;
    }

    const set<string>& getPeerList() const
    {
        return peer_list_;
    }

    string getTunnelName() const
    {
        return tunnel_;
    }

    uint32_t getVni() const
    {
        return vni_;
    }

    string getScope() const
    {
        return scope_;
    }

    bool getAdvertisePrefix() const
    {
        return advertise_prefix_;
    }

    swss::MacAddress getOverlayDMac() const
    {
        return overlay_dmac_;
    }

    void setOverlayDMac(swss::MacAddress mac_addr)
    {
        overlay_dmac_ = mac_addr;
    }

    virtual ~VNetObject() noexcept(false) {};

private:
    set<string> peer_list_ = {};
    string tunnel_;
    uint32_t vni_;
    string scope_;
    bool advertise_prefix_;
    swss::MacAddress overlay_dmac_; 
};

struct nextHop
{
    IpAddresses ips;
    string ifname;
};

typedef std::map<IpPrefix, NextHopGroupKey> TunnelRoutes;
typedef std::map<IpPrefix, nextHop> RouteMap;
typedef std::map<IpPrefix, string> ProfileMap;

class VNetVrfObject : public VNetObject
{
public:
    VNetVrfObject(const string& vnet, const VNetInfo& vnetInfo, vector<sai_attribute_t>& attrs);

    sai_object_id_t getVRidIngress() const;

    sai_object_id_t getVRidEgress() const;

    set<sai_object_id_t> getVRids() const;

    sai_object_id_t getEncapMapId() const
    {
        return getVRidIngress();
    }

    sai_object_id_t getDecapMapId() const
    {
        if (std::find(vr_cntxt.begin(), vr_cntxt.end(), VR_TYPE::EGR_VR_VALID) != vr_cntxt.end())
        {
            return getVRidEgress();
        }
        else
        {
            return getVRidIngress();
        }
    }

    sai_object_id_t getVRid() const
    {
        return getVRidIngress();
    }

    bool createObj(vector<sai_attribute_t>&);

    bool updateObj(vector<sai_attribute_t>&);

    bool addRoute(IpPrefix& ipPrefix, NextHopGroupKey& nexthops);
    bool addRoute(IpPrefix& ipPrefix, nextHop& nh, bool increaseRefCount = true);
    bool removeRoute(IpPrefix& ipPrefix, bool decreaseRefCount = true);

    void addProfile(IpPrefix& ipPrefix, string& profile);
    void removeProfile(IpPrefix& ipPrefix);
    string getProfile(IpPrefix& ipPrefix);

    size_t getRouteCount() const;
    bool getRouteNextHop(IpPrefix& ipPrefix, nextHop& nh);
    bool hasRoute(IpPrefix& ipPrefix);

    sai_object_id_t getTunnelNextHop(NextHopKey& nh);
    bool removeTunnelNextHop(NextHopKey& nh);
    void increaseNextHopRefCount(const nextHop&);
    void decreaseNextHopRefCount(const nextHop&);

    const RouteMap &getRouteMap() const { return routes_; }
    const TunnelRoutes &getTunnelRoutes() const { return tunnels_; }

    ~VNetVrfObject();

private:
    string vnet_name_;
    vrid_list_t vr_ids_;

    TunnelRoutes tunnels_;
    RouteMap routes_;
    ProfileMap profile_;
};

typedef std::unique_ptr<VNetObject> VNetObject_T;
typedef std::unordered_map<std::string, VNetObject_T> VNetTable;

class VNetOrch : public Orch2
{
public:
    VNetOrch(DBConnector *db, const std::string&, VNET_EXEC op = VNET_EXEC::VNET_EXEC_VRF);

    bool setIntf(const string& alias, const string name, const IpPrefix *prefix = nullptr, const bool adminUp = true, const uint32_t mtu = 0);
    bool delIntf(const string& alias, const string name, const IpPrefix *prefix = nullptr);

    bool isVnetExists(const std::string& name) const
    {
        return vnet_table_.find(name) != std::end(vnet_table_);
    }

    template <class T>
    T* getTypePtr(const std::string& name) const
    {
        return static_cast<T *>(vnet_table_.at(name).get());
    }

    const set<string>& getPeerList(const std::string& name) const
    {
        return vnet_table_.at(name)->getPeerList();
    }

    string getTunnelName(const std::string& name) const
    {
        return vnet_table_.at(name)->getTunnelName();
    }

    bool getAdvertisePrefix(const std::string& name) const
    {
        return vnet_table_.at(name)->getAdvertisePrefix();
    }

    bool isVnetExecVrf() const
    {
        return (vnet_exec_ == VNET_EXEC::VNET_EXEC_VRF);
    }

    bool isVnetExecBridge() const
    {
        return (vnet_exec_ == VNET_EXEC::VNET_EXEC_BRIDGE);
    }

    bool getVrfIdByVnetName(const std::string& vnet_name, sai_object_id_t &vrf_id);
    bool getVnetNameByVrfId(sai_object_id_t vrf_id, std::string& vnet_name);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    template <class T>
    std::unique_ptr<T> createObject(const string&, const VNetInfo&, vector<sai_attribute_t>&);

    VNetTable vnet_table_;
    VNetRequest request_;
    VNET_EXEC vnet_exec_;

};

const request_description_t vnet_route_description = {
    { REQ_T_STRING, REQ_T_IP_PREFIX },
    {
        { "endpoint",               REQ_T_IP_LIST },
        { "ifname",                 REQ_T_STRING },
        { "nexthop",                REQ_T_STRING },
        { "vni",                    REQ_T_STRING },
        { "mac_address",            REQ_T_STRING },
        { "endpoint_monitor",       REQ_T_IP_LIST },
        { "profile",                REQ_T_STRING },
        { "primary",                REQ_T_IP_LIST },
        { "monitoring",             REQ_T_STRING },
        { "adv_prefix",             REQ_T_IP_PREFIX },
        { "check_directly_connected", REQ_T_BOOL },
        { "rx_monitor_timer",       REQ_T_UINT },
        { "tx_monitor_timer",       REQ_T_UINT },
        { "metric",                 REQ_T_UINT }
    },
    { }
};

const request_description_t monitor_state_request_description = {
            { REQ_T_IP, REQ_T_IP_PREFIX, },
            {
                { "state",  REQ_T_STRING },
            },
            { "state" }
};

const request_description_t custom_bfd_request_description = {
            { REQ_T_STRING, REQ_T_STRING, REQ_T_IP, },
            {
                { "type",               REQ_T_STRING },
                { "async_active",       REQ_T_STRING },
                { "local_discriminator", REQ_T_STRING },
                { "local_addr",         REQ_T_IP },
                { "tx_interval",        REQ_T_UINT },
                { "rx_interval",        REQ_T_UINT },
                { "multiplier",         REQ_T_UINT },
                { "multihop",           REQ_T_BOOL },
                { "state",              REQ_T_STRING },
            },
            { }
};

class MonitorStateRequest : public Request
{
public:
    MonitorStateRequest() : Request(monitor_state_request_description, '|') { }
};

class MonitorOrch : public Orch2
{
public:
    MonitorOrch(swss::DBConnector *db, std::string tableName);
    virtual ~MonitorOrch(void);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    MonitorStateRequest request_;
};

class CustomBfdRequest : public Request
{
public:
    CustomBfdRequest() : Request(custom_bfd_request_description, '|') { }
};

class BfdMonitorOrch : public Orch2
{
public:
    BfdMonitorOrch(swss::DBConnector *db, std::string tableName);
    virtual ~BfdMonitorOrch(void);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    CustomBfdRequest request_;
};

class VNetRouteRequest : public Request
{
public:
    VNetRouteRequest() : Request(vnet_route_description, ':', true) { }
};

struct VNetNextHopUpdate
{
    std::string op;
    std::string vnet;
    IpAddress destination;
    IpPrefix prefix;
    nextHop nexthop;
};
/* VNetEntry: vnet name, next hop IP address(es)  */
typedef std::map<std::string, nextHop> VNetEntry;
/* VNetRouteTable: destination network, vnet name, next hop IP address(es) */
typedef std::map<IpPrefix, VNetEntry > VNetRouteTable;
struct VNetNextHopObserverEntry
{
    VNetRouteTable routeTable;
    list<Observer*> observers;
};
/* NextHopObserverTable: Destination IP address, next hop observer entry */
typedef std::map<IpAddress, VNetNextHopObserverEntry> VNetNextHopObserverTable;

struct VNetNextHopInfo
{
    IpAddress monitor_addr;
    sai_bfd_session_state_t bfd_state;
    int ref_count;
};

struct BfdSessionInfo
{
    sai_bfd_session_state_t bfd_state;
    std::string vnet;
    NextHopKey endpoint;

    bool custom_bfd = false;
};

struct MonitorSessionInfo
{
    std::string monitoring_type = VNET_MONITORING_TYPE_CUSTOM;
    sai_bfd_session_state_t custom_bfd_state;
    monitor_session_state_t state;
    NextHopKey endpoint;
    int ref_count;
};

struct MonitorUpdate
{
    std::string monitoring_type = VNET_MONITORING_TYPE_CUSTOM;
    sai_bfd_session_state_t custom_bfd_state;
    monitor_session_state_t state;
    IpAddress monitor;
    IpPrefix prefix;
    std::string vnet;
};

struct VNetTunnelRouteEntry
{
    // The nhg_key is the key for the next hop group which is currently active in hardware.
    // For priority routes, this can be a subset of eith primary or secondary NHG or an empty NHG.
    NextHopGroupKey nhg_key;
    // For regular Ecmp rotues the priamry and secondary fields wil lbe empty. For priority
    // routes they wil lcontain the origna lprimary and secondary NHGs.
    NextHopGroupKey primary;
    NextHopGroupKey secondary;
};

struct VNetLocEpAclRule
{
    swss::IpPrefix vip;
    swss::IpAddress nh_ip;
    std::string rule_name;
};

typedef std::map<NextHopGroupKey, NextHopGroupInfo> VNetNextHopGroupInfoTable;
typedef std::map<IpPrefix, VNetTunnelRouteEntry> VNetTunnelRouteTable;
typedef std::map<IpAddress, BfdSessionInfo> BfdSessionTable;
typedef std::map<IpPrefix, std::map<IpAddress, MonitorSessionInfo>> MonitorSessionTable;
typedef std::map<IpAddress, VNetNextHopInfo> VNetEndpointInfoTable;

class VNetTunnelTermAcl
{
public:
    VNetTunnelTermAcl(DBConnector *cfgDb, DBConnector *appDb);

    bool createAclRule(const string vnet_name, swss::IpPrefix& vip, swss::IpAddress nh_ip);
    bool removeAclRule(const string vnet_name, swss::IpPrefix& vip);
    std::function<std::string(const std::string&, const std::string&)> concat =
        [](const std::string &a, const std::string &b) { return a + "," + b; };
    bool getAclRule(const string vnet_name, const swss::IpPrefix& vip, VNetLocEpAclRule& rule_found);

protected:

    void lazyInit();

    std::shared_ptr<TunnelTermHelper> ctx_;

    bool acl_table_initialized_ = false;
    unique_ptr<swss::ProducerStateTable> acl_table_;
    unique_ptr<swss::ProducerStateTable> acl_table_type_;
    unique_ptr<swss::ProducerStateTable> acl_rule_table_;
    std::map<std::string, std::vector<VNetLocEpAclRule>> vnet_loc_ep_acl_rule_map_;
};

class VNetRouteOrch : public Orch2, public Subject, public Observer
{
public:
    VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *);

    typedef pair<string, bool (VNetRouteOrch::*) (const Request& )> handler_pair;
    typedef map<string, bool (VNetRouteOrch::*) (const Request& )> handler_map;

    void attach(Observer* observer, const IpAddress& dstAddr);
    void detach(Observer* observer, const IpAddress& dstAddr);

    void update(SubjectType, void *);
    void updateMonitorState(string& op, const IpPrefix& prefix , const IpAddress& endpoint, string state);
    void updateCustomBfdState(const IpAddress& monitoring_ip, const string& state);
    void updateAllMonitoringSession(const string& vnet);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    void addRoute(const std::string & vnet, const IpPrefix & ipPrefix, const nextHop& nh);
    void delRoute(const IpPrefix& ipPrefix);

    bool handleRoutes(const Request&);
    bool handleTunnel(const Request&);

    bool hasNextHopGroup(const string&, const NextHopGroupKey&);
    sai_object_id_t getNextHopGroupId(const string&, const NextHopGroupKey&);
    bool addNextHopGroup(const string&, const NextHopGroupKey&, VNetVrfObject *vrf_obj,
                            const string& monitoring, const bool isLocalEp=false);
    bool removeNextHopGroup(const string&, const NextHopGroupKey&, VNetVrfObject *vrf_obj);
    bool createNextHopGroup(const string&, NextHopGroupKey&, VNetVrfObject *vrf_obj,
                            const string& monitoring);
    NextHopGroupKey getActiveNHSet(const string&, NextHopGroupKey&, const IpPrefix& );

    bool selectNextHopGroup(const string&, NextHopGroupKey&, NextHopGroupKey&, const string&, const int32_t, const int32_t, IpPrefix&,
                            VNetVrfObject *vrf_obj, NextHopGroupKey&,
                            const std::map<NextHopKey,IpAddress>& monitors=std::map<NextHopKey, IpAddress>());

    void createBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& ipAddr, const int32_t rx_monitor_timer, const int32_t tx_monitor_timer);
    void removeBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& ipAddr);
    void createCustomBFDMonitoringSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& monitor_addr, IpPrefix& ipPrefix, const int32_t rx_monitor_timer, const int32_t tx_monitor_timer);
    void createMonitoringSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& ipAddr, IpPrefix& ipPrefix);
    void removeMonitoringSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& ipAddr, IpPrefix& ipPrefix);
    void setEndpointMonitor(const string& vnet, const map<NextHopKey, IpAddress>& monitors, NextHopGroupKey& nexthops,
                            const string& monitoring, const int32_t rx_monitor_timer, const int32_t tx_monitor_timer,
                            IpPrefix& ipPrefix);
    void delEndpointMonitor(const string& vnet, NextHopGroupKey& nexthops, IpPrefix& ipPrefix);
    void postRouteState(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops, string& profile);
    void removeRouteState(const string& vnet, IpPrefix& ipPrefix);
    void addRouteAdvertisement(IpPrefix& ipPrefix, string& profile);
    void removeRouteAdvertisement(IpPrefix& ipPrefix);

    void updateVnetTunnel(const BfdUpdate&);
    void updateVnetTunnelCustomMonitor(const MonitorUpdate& update);
    bool updateTunnelRoute(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops, string& op);
    void createSubnetDecapTerm(const IpPrefix &ipPrefix);
    void removeSubnetDecapTerm(const IpPrefix &ipPrefix);

    bool setAndDeleteRoutesWithRouteOrch(const sai_object_id_t vr_id, const IpPrefix& ipPrefix,
                                        const NextHopGroupKey& nhg, const string& op);

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops, string& op, string& profile,
                    const string& monitoring, const int32_t rx_monitor_timer, const int32_t tx_monitor_timer,
                    NextHopGroupKey& nexthops_secondary, const IpPrefix& adv_prefix,
                    const std::map<NextHopKey, IpAddress>& monitors=std::map<NextHopKey, IpAddress>());

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, nextHop& nh, string& op);

    bool isLocalEndpoint(const string&vnet, const IpAddress &ipAddr);
    bool isPartiallyLocal(const std::vector<swss::IpAddress>& ip_list);

    VNetOrch *vnet_orch_;
    VNetRouteRequest request_;
    handler_map handler_map_;

    VNetRouteTable syncd_routes_;
    VNetNextHopObserverTable next_hop_observers_;
    std::map<std::string, VNetNextHopGroupInfoTable> syncd_nexthop_groups_;
    std::map<std::string, VNetTunnelRouteTable> syncd_tunnel_routes_;
    std::map<std::string, bool> vnet_tunnel_route_check_directly_connected;
    BfdSessionTable bfd_sessions_;
    std::map<std::string, MonitorSessionTable> monitor_info_;
    std::map<std::string, VNetEndpointInfoTable> nexthop_info_;
    std::map<IpPrefix, IpPrefix> prefix_to_adv_prefix_;
    std::map<IpPrefix, int> adv_prefix_refcount_;
    std::set<IpPrefix> subnet_decap_terms_created_;
    ProducerStateTable bfd_session_producer_;
    ProducerStateTable app_tunnel_decap_term_producer_;
    unique_ptr<Table> monitor_session_producer_;
    shared_ptr<DBConnector> config_db_;
    shared_ptr<DBConnector> state_db_;
    shared_ptr<DBConnector> app_db_;
    unique_ptr<Table> state_vnet_rt_tunnel_table_;
    unique_ptr<Table> state_vnet_rt_adv_table_;

    shared_ptr<VNetTunnelTermAcl> vnet_tunnel_term_acl_;
};

class VNetCfgRouteOrch : public Orch
{
public:
    VNetCfgRouteOrch(DBConnector *db, DBConnector *appDb, vector<string> &tableNames);
    using Orch::doTask;

private:
    void doTask(Consumer &consumer);

    bool doVnetTunnelRouteTask(const KeyOpFieldsValuesTuple & t, const std::string & op);
    bool doVnetRouteTask(const KeyOpFieldsValuesTuple & t, const std::string & op);

    ProducerStateTable m_appVnetRouteTable, m_appVnetRouteTunnelTable;
};

#endif // __VNETORCH_H
