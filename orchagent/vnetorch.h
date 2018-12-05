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
    },
    { "vxlan_tunnel", "vni" } // mandatory attributes
};

enum class VR_TYPE
{
    ING_VR_VALID,
    EGR_VR_VALID,
    VR_INVALID
};

struct tunnelEndpoint
{
    IpAddress ip;
    MacAddress mac;
    uint32_t vni;
};

struct VNetInfo
{
    string tunnel;
    uint32_t vni;
    MacAddress mac;
    set<string> peers;
};

typedef map<VR_TYPE, sai_object_id_t> vrid_list_t;
typedef map<IpAddress, sai_object_id_t> NextHopMap;
extern std::vector<VR_TYPE> vr_cntxt;

class VNetRequest : public Request
{
public:
    VNetRequest() : Request(vnet_request_description, ':') { }
};

class VNetOrch;

class VNetObject
{
public:
    VNetObject(const string& vnetName, VNetOrch *vnetOrch, const VNetInfo& vnetInfo) :
        vnet_name_(vnetName),
        vnet_orch_(vnetOrch),
        tunnel_(vnetInfo.tunnel),
        vni_(vnetInfo.vni),
        peer_list_(vnetInfo.peers),
        mac_(vnetInfo.mac)
    { }

    virtual bool updateObj(const VNetInfo& vnetInfo) = 0;

    virtual bool addIntf(Port& port, IpPrefix *prefix)
    {
        return false;
    }

    virtual bool addRoute(IpPrefix& ipPrefix, string& ifname)
    {
        return false;
    }

    virtual bool addTunnelRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp)
    {
        return false;
    }

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

    string getName() const
    {
        return vnet_name_;
    }

    uint32_t getVni() const
    {
        return vni_;
    }

    const MacAddress& getSrcMac() const
    {
        return mac_;
    }

    VNetOrch *getVnetOrch() const
    {
        return vnet_orch_;
    }

    virtual ~VNetObject() {};

private:
    string vnet_name_;
    VNetOrch *vnet_orch_;
    set<string> peer_list_ = {};
    string tunnel_;
    uint32_t vni_;
    MacAddress mac_;
};

class VNetVrfObject : public VNetObject
{
public:
    VNetVrfObject(const string& vnetName, VNetOrch *vnetOrch, const VNetInfo& vnetInfo);

    virtual bool addRoute(IpPrefix& ipPrefix, string& ifname);

    virtual bool addTunnelRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp);

    bool createObj();

    virtual bool updateObj(const VNetInfo& vnetInfo);

    ~VNetVrfObject();

private:
    sai_object_id_t getVRidIngress() const;

    sai_object_id_t getVRidEgress() const;

    set<sai_object_id_t> getVRids() const;

    bool add_route(sai_object_id_t vr_id, sai_ip_prefix_t& ip_pfx, sai_object_id_t nh_id);

    sai_object_id_t getNextHop(tunnelEndpoint& endp);

    vrid_list_t vr_ids_;
    NextHopMap nh_map_;
};

struct VnetBridgeInfo
{
    sai_object_id_t bridge_id;
    sai_object_id_t bridge_port_rif_id;
    sai_object_id_t bridge_port_tunnel_id;
    sai_object_id_t rif_id;
};

class VNetBitmapObject: public VNetObject
{
public:
    VNetBitmapObject(const string& vnetName, VNetOrch *vnetOrch, const VNetInfo& vnetInfo);

    virtual bool addIntf(Port& port, IpPrefix *prefix);

    virtual bool updateObj(const VNetInfo& vnetInfo);

    virtual bool addTunnelRoute(IpPrefix& ipPrefix, tunnelEndpoint& endp);

    virtual ~VNetBitmapObject() {}

private:
    static uint32_t getFreeBitmapId(const string& name);

    static uint32_t getBitmapId(const string& name);

    static void recycleBitmapId(uint32_t id);

    static uint32_t getFreeVnetTableOffset();

    static void recycleVnetTableOffset(uint32_t offset);

    static uint32_t getFreeTunnelRouteTableOffset();

    static void recycleTunnelRouteTableOffset(uint32_t offset);

    static VnetBridgeInfo getBridgeInfoByVni(uint32_t vni, string tunnelName);

    bool addVlan(uint16_t vlan_id);

    static uint32_t vnetBitmap_;
    static map<string, uint32_t> vnetIds_;
    static set<uint32_t> vnetOffsets_;
    static set<uint32_t> tunnelOffsets_;
    static map<uint32_t, VnetBridgeInfo> bridgeInfoMap_;

    uint32_t vnet_id_;
};

typedef std::unique_ptr<VNetObject> VNetObject_T;
typedef std::unordered_map<std::string, VNetObject_T> VNetTable;

class VNetOrch : public Orch2
{
public:
    VNetOrch(DBConnector *db, const std::string&);
    virtual ~VNetOrch() {}

    bool isVnetExists(const std::string& name) const
    {
        return vnet_table_.find(name) != std::end(vnet_table_);
    }

    VNetObject * getVnetPtr(const string& name)
    {
        return vnet_table_.at(name).get();
    }

    const set<string>& getPeerList(const std::string& name) const
    {
        return vnet_table_.at(name)->getPeerList();
    }

    string getTunnelName(const std::string& name) const
    {
        return vnet_table_.at(name)->getTunnelName();
    }

    virtual std::unique_ptr<VNetObject> createObject(const string& vnet_name, const VNetInfo& vnetInfo) = 0;

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VNetTable vnet_table_;
    VNetRequest request_;
};

class VNetVrfOrch : public VNetOrch
{
public:
    VNetVrfOrch(DBConnector *db, const std::string&);
    virtual ~VNetVrfOrch() {}

    virtual std::unique_ptr<VNetObject> createObject(const string& vnet_name, const VNetInfo& vnetInfo);
};

class VNetBitmapOrch : public VNetOrch
{
public:
    VNetBitmapOrch(DBConnector *db, const std::string&);
    virtual ~VNetBitmapOrch() {}

    virtual std::unique_ptr<VNetObject> createObject(const string& vnet_name, const VNetInfo& vnetInfo);
};

const request_description_t vnet_route_description = {
    { REQ_T_STRING, REQ_T_IP_PREFIX },
    {
        { "endpoint",    REQ_T_IP },
        { "ifname",      REQ_T_STRING },
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

    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, tunnelEndpoint& endp);

    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, string& ifname);

    VNetOrch *vnet_orch_;
    VNetRouteRequest request_;
    handler_map handler_map_;
};

#endif // __VNETORCH_H
