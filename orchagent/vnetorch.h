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
        { "vnet_name",     REQ_T_STRING },
        { "peer_list",     REQ_T_SET },
    },
    { } // no mandatory attributes
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

using vrid_list_t = map<VR_TYPE, sai_object_id_t>;
extern std::vector<VR_TYPE> vr_cntxt;

class VNetRequest : public Request
{
public:
    VNetRequest() : Request(vnet_request_description, ':') { }
};

class VNetObject
{
public:
    VNetObject(set<string>& p_list)
    {
        peer_list_ = p_list;
    }

    virtual sai_object_id_t getEncapMapId() const = 0;

    virtual sai_object_id_t getDecapMapId() const = 0;

    virtual bool updateObj(vector<sai_attribute_t>&) = 0;

    void setPeerList(set<string>& p_list)
    {
        peer_list_ = p_list;
    }

    virtual sai_object_id_t getVRid() const = 0;

    const set<string>& getPeerList() const
    {
        return peer_list_;
    }

    virtual ~VNetObject() {};

private:
    set<string> peer_list_ = {};
};

class VNetVrfObject : public VNetObject
{
public:
    VNetVrfObject(const std::string& name, set<string>& p_list, vector<sai_attribute_t>& attrs);

    sai_object_id_t getVRidIngress() const;

    sai_object_id_t getVRidEgress() const;

    set<sai_object_id_t> getVRids() const;

    virtual sai_object_id_t getEncapMapId() const
    {
        return getVRidIngress();
    }

    virtual sai_object_id_t getDecapMapId() const
    {
        return getVRidEgress();
    }

    virtual sai_object_id_t getVRid() const
    {
        return getVRidIngress();
    }

    bool createObj(vector<sai_attribute_t>&);

    bool updateObj(vector<sai_attribute_t>&);

    ~VNetVrfObject();

private:
    string vnet_name_;
    vrid_list_t vr_ids_;
};

using VNetObject_T = std::unique_ptr<VNetObject>;
typedef std::unordered_map<std::string, VNetObject_T> VNetTable;

class VNetOrch : public Orch2
{
public:
    VNetOrch(DBConnector *db, const std::string&, VNET_EXEC op = VNET_EXEC::VNET_EXEC_VRF);

    bool isVnetExists(const std::string& name) const
    {
        return vnet_table_.find(name) != std::end(vnet_table_);
    }

    template <class T>
    T* getTypePtr(const std::string& name) const
    {
        return static_cast<T *>(vnet_table_.at(name).get());
    }

    sai_object_id_t getEncapMapId(const std::string& name) const
    {
        return vnet_table_.at(name)->getEncapMapId();
    }

    sai_object_id_t getDecapMapId(const std::string& name) const
    {
        return vnet_table_.at(name)->getDecapMapId();
    }

    const set<string>& getPeerList(const std::string& name) const
    {
        return vnet_table_.at(name)->getPeerList();
    }

    sai_object_id_t getVRid(const std::string& name) const
    {
        return vnet_table_.at(name)->getVRid();
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
    std::unique_ptr<T> createObject(const string&, set<string>&, vector<sai_attribute_t>&);

    VNetTable vnet_table_;
    VNetRequest request_;
    VNET_EXEC vnet_exec_;

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

using NextHopMap = map<IpAddress, sai_object_id_t>;
using NextHopTunnels = map<string, NextHopMap>;

struct endpoint
{
    IpAddress ip;
    MacAddress mac;
    uint32_t vni;
};

class VNetRouteOrch : public Orch2
{
public:
    VNetRouteOrch(DBConnector *db, vector<string> &tableNames, VNetOrch *);
    using handler_pair = pair<string, void (VNetRouteOrch::*) (const Request& )>;
    using handler_map = map<string, void (VNetRouteOrch::*) (const Request& )>;

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    void handleRoutes(const Request&);
    void handleTunnel(const Request&);

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, endpoint& endp);

    template<typename T>
    bool doRouteTask(const string& vnet, IpPrefix& ipPrefix, string& ifname);

    sai_object_id_t getNextHop(const string& vnet, endpoint& endp);

    VNetOrch *vnet_orch_;
    VNetRouteRequest request_;
    handler_map handler_map_;
    NextHopTunnels nh_tunnels_;
};

#endif // __VNETORCH_H
