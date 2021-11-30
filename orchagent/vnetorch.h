#ifndef __VNETORCH_H
#define __VNETORCH_H

#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <bitset>
#include <tuple>

#include "request_parser.h"
#include "ipaddresses.h"
#include "producerstatetable.h"
#include "observer.h"
#include "nexthopgroupkey.h"
#include "bfdorch.h"

#define VNET_BITMAP_SIZE 32
#define VNET_TUNNEL_SIZE 40960
#define VNET_ROUTE_FULL_MASK_OFFSET_MAX 3000
#define VNET_NEIGHBOR_MAX 0xffff
#define VXLAN_ENCAP_TTL 128
#define VNET_BITMAP_RIF_MTU 9100

extern sai_object_id_t gVirtualRouterId;

const request_description_t vnet_request_description = {
    { REQ_T_STRING },
    {
        { "src_mac",       REQ_T_MAC_ADDRESS },
        { "vxlan_tunnel",  REQ_T_STRING },
        { "vni",           REQ_T_UINT },
        { "peer_list",     REQ_T_SET },
        { "guid",          REQ_T_STRING },
        { "scope",         REQ_T_STRING },
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
               scope_(vnetInfo.scope)
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

    virtual ~VNetObject() noexcept(false) {};

private:
    set<string> peer_list_ = {};
    string tunnel_;
    uint32_t vni_;
    string scope_;
};

struct nextHop
{
    IpAddresses ips;
    string ifname;
};

typedef std::map<IpPrefix, NextHopGroupKey> TunnelRoutes;
typedef std::map<IpPrefix, nextHop> RouteMap;

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
    bool addRoute(IpPrefix& ipPrefix, nextHop& nh);
    bool removeRoute(IpPrefix& ipPrefix);

    size_t getRouteCount() const;
    bool getRouteNextHop(IpPrefix& ipPrefix, nextHop& nh);
    bool hasRoute(IpPrefix& ipPrefix);

    sai_object_id_t getTunnelNextHop(NextHopKey& nh);
    bool removeTunnelNextHop(NextHopKey& nh);
    void increaseNextHopRefCount(const nextHop&);
    void decreaseNextHopRefCount(const nextHop&);

    ~VNetVrfObject();

private:
    string vnet_name_;
    vrid_list_t vr_ids_;

    TunnelRoutes tunnels_;
    RouteMap routes_;
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

    bool isVnetExecVrf() const
    {
        return (vnet_exec_ == VNET_EXEC::VNET_EXEC_VRF);
    }

    bool isVnetExecBridge() const
    {
        return (vnet_exec_ == VNET_EXEC::VNET_EXEC_BRIDGE);
    }

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
    },
    { }
};

class VNetRouteRequest : public Request
{
public:
    VNetRouteRequest() : Request(vnet_route_description, ':') { }
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
};

typedef std::map<NextHopGroupKey, NextHopGroupInfo> VNetNextHopGroupInfoTable;
typedef std::map<IpPrefix, NextHopGroupKey> VNetTunnelRouteTable;
typedef std::map<IpAddress, BfdSessionInfo> BfdSessionTable;
typedef std::map<IpAddress, VNetNextHopInfo> VNetEndpointInfoTable;

class VNetRouteOrch : public Orch2, public Subject, public Observer
{
public:
    VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *);

    typedef pair<string, bool (VNetRouteOrch::*) (const Request& )> handler_pair;
    typedef map<string, bool (VNetRouteOrch::*) (const Request& )> handler_map;

    void attach(Observer* observer, const IpAddress& dstAddr);
    void detach(Observer* observer, const IpAddress& dstAddr);

    void update(SubjectType, void *);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    void addRoute(const std::string & vnet, const IpPrefix & ipPrefix, const nextHop& nh);
    void delRoute(const IpPrefix& ipPrefix);

    bool handleRoutes(const Request&);
    bool handleTunnel(const Request&);

    bool hasNextHopGroup(const string&, const NextHopGroupKey&);
    sai_object_id_t getNextHopGroupId(const string&, const NextHopGroupKey&);
    bool addNextHopGroup(const string&, const NextHopGroupKey&, VNetVrfObject *vrf_obj);
    bool removeNextHopGroup(const string&, const NextHopGroupKey&, VNetVrfObject *vrf_obj);

    void createBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& ipAddr);
    void removeBfdSession(const string& vnet, const NextHopKey& endpoint, const IpAddress& ipAddr);
    void setEndpointMonitor(const string& vnet, const map<NextHopKey, IpAddress>& monitors, NextHopGroupKey& nexthops);
    void delEndpointMonitor(const string& vnet, NextHopGroupKey& nexthops);
    void postRouteState(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops);
    void removeRouteState(const string& vnet, IpPrefix& ipPrefix);

    void updateVnetTunnel(const BfdUpdate&);
    bool updateTunnelRoute(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops, string& op);

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, NextHopGroupKey& nexthops, string& op,
                    const std::map<NextHopKey, IpAddress>& monitors=std::map<NextHopKey, IpAddress>());

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, nextHop& nh, string& op);

    VNetOrch *vnet_orch_;
    VNetRouteRequest request_;
    handler_map handler_map_;

    VNetRouteTable syncd_routes_;
    VNetNextHopObserverTable next_hop_observers_;
    std::map<std::string, VNetNextHopGroupInfoTable> syncd_nexthop_groups_;
    std::map<std::string, VNetTunnelRouteTable> syncd_tunnel_routes_;
    BfdSessionTable bfd_sessions_;
    std::map<std::string, VNetEndpointInfoTable> nexthop_info_;
    ProducerStateTable bfd_session_producer_;
    shared_ptr<DBConnector> state_db_;
    unique_ptr<Table> state_vnet_rt_tunnel_table_;
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
