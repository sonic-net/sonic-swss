#ifndef __VNETORCH_H
#define __VNETORCH_H

#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>

#include "request_parser.h"

extern sai_object_id_t gVirtualRouterId;

const request_description_t vnet_request_description = {
    { REQ_T_STRING },
    {
        { "src_mac",       REQ_T_MAC_ADDRESS },
        { "vxlan_tunnel",  REQ_T_STRING },
        { "vni",           REQ_T_UINT },
        { "peer_list",     REQ_T_SET },
        { "guid",          REQ_T_STRING },
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

typedef map<VR_TYPE, sai_object_id_t> vrid_list_t;
extern std::vector<VR_TYPE> vr_cntxt;

class VNetRequest : public Request
{
public:
    VNetRequest() : Request(vnet_request_description, ':') { }
};

class VNetObject
{
public:
    VNetObject(string& tunName, set<string>& peer, uint32_t vni) : tunnel_(tunName), peer_list_(peer), vni_(vni) { }

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

    virtual ~VNetObject() {};

private:
    set<string> peer_list_ = {};
    string tunnel_;
    uint32_t vni_;
};

struct tunnelEndpoint
{
    IpAddress ip;
    MacAddress mac;
    uint32_t vni;
};

struct nextHop
{
    IpAddress ip;
    string ifname;
};

typedef std::map<IpPrefix, tunnelEndpoint> TunnelRoutes;
typedef std::map<IpPrefix, nextHop> RouteMap;

class VNetVrfObject : public VNetObject
{
public:
    VNetVrfObject(const string& vnet, string& tunnel, set<string>& peer, uint32_t vni, vector<sai_attribute_t>& attrs);

    sai_object_id_t getVRidIngress() const;

    sai_object_id_t getVRidEgress() const;

    set<sai_object_id_t> getVRids() const;

    sai_object_id_t getEncapMapId() const
    {
        return getVRidIngress();
    }

    sai_object_id_t getDecapMapId() const
    {
        return getVRidEgress();
    }

    sai_object_id_t getVRid() const
    {
        return getVRidIngress();
    }

    bool createObj(vector<sai_attribute_t>&);

    bool updateObj(vector<sai_attribute_t>&);

    bool addRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp);
    bool addRoute(IpPrefix& ipPrefix, nextHop& nh);
    bool removeRoute(IpPrefix& ipPrefix);

    size_t getRouteCount() const;
    bool getRouteNH(IpPrefix& ipPrefix, nextHop& nh);
    bool hasRoute(IpPrefix& ipPrefix);

    sai_object_id_t getNextHop(tunnelEndpoint& endp);
    bool removeNextHop(tunnelEndpoint& endp);

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

    bool setIntf(const string& alias, const string vnet_name, const IpPrefix *prefix = nullptr);

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
    std::unique_ptr<T> createObject(const string&, string&, set<string>&, uint32_t, vector<sai_attribute_t>&);

    VNetTable vnet_table_;
    VNetRequest request_;
    VNET_EXEC vnet_exec_;

};

const request_description_t vnet_route_description = {
    { REQ_T_STRING, REQ_T_IP_PREFIX },
    {
        { "endpoint",    REQ_T_IP },
        { "ifname",      REQ_T_STRING },
        { "nexthop",     REQ_T_IP },
        { "vni",         REQ_T_UINT },
        { "mac_address", REQ_T_MAC_ADDRESS },
    },
    { }
};

class VNetRouteRequest : public Request
{
public:
    VNetRouteRequest() : Request(vnet_route_description, ':') { }
};

class VNetRouteOrch : public Orch2
{
public:
    VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *);

    typedef pair<string, void (VNetRouteOrch::*) (const Request& )> handler_pair;
    typedef map<string, void (VNetRouteOrch::*) (const Request& )> handler_map;

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    void handleRoutes(const Request&);
    void handleTunnel(const Request&);

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, tunnelEndpoint& endp, string& op);

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, nextHop& nh, string& op);

    VNetOrch *vnet_orch_;
    VNetRouteRequest request_;
    handler_map handler_map_;
};

#endif // __VNETORCH_H
