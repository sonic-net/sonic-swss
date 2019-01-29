#pragma once

#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include "request_parser.h"
#include "portsorch.h"
#include "vrforch.h"

enum class MAP_T
{
    MAP_TO_INVALID,
    VNI_TO_VLAN_ID,
    VLAN_ID_TO_VNI,
    VRID_TO_VNI,
    VNI_TO_VRID,
    BRIDGE_TO_VNI,
    VNI_TO_BRIDGE
};

struct tunnel_ids_t
{
    sai_object_id_t tunnel_encap_id;
    sai_object_id_t tunnel_decap_id;
    sai_object_id_t tunnel_id;
    sai_object_id_t tunnel_term_id;
};

struct nh_key_t
{
    IpAddress ip_addr;
    MacAddress mac_address;
    uint32_t vni=0;

    nh_key_t() = default;

    nh_key_t(IpAddress ipAddr, MacAddress macAddress=MacAddress(), uint32_t vnId=0)
    {
        ip_addr = ipAddr;
        mac_address = macAddress;
        vni = vnId;
    };

    bool operator== (const nh_key_t& rhs) const
    {
        if (!(ip_addr == rhs.ip_addr) || mac_address != rhs.mac_address || vni != rhs.vni)
        {
            return false;
        }
        return true;
    }
};

struct nh_key_hash
{
    size_t operator() (const nh_key_t& key) const
    {
        stringstream ss;
        ss << key.ip_addr.to_string() << key.mac_address.to_string() << std::to_string(key.vni);
        return std::hash<std::string>() (ss.str());
    }
};

struct nh_tunnel_t
{
    sai_object_id_t nh_id;
    int             ref_count;
};

typedef std::map<uint32_t, std::pair<sai_object_id_t, sai_object_id_t>> TunnelMapEntries;
typedef std::unordered_map<nh_key_t, nh_tunnel_t, nh_key_hash> TunnelNHs;

class VxlanTunnel
{
public:
    VxlanTunnel(string name, IpAddress srcIp, IpAddress dstIp)
                :tunnel_name_(name), src_ip_(srcIp), dst_ip_(dstIp) { }

    bool isActive() const
    {
        return active_;
    }

    bool createTunnel(MAP_T encap, MAP_T decap);
    sai_object_id_t addEncapMapperEntry(sai_object_id_t obj, uint32_t vni);
    sai_object_id_t addDecapMapperEntry(sai_object_id_t obj, uint32_t vni);

    void insertMapperEntry(sai_object_id_t encap, sai_object_id_t decap, uint32_t vni);
    std::pair<sai_object_id_t, sai_object_id_t> getMapperEntry(uint32_t vni);

    sai_object_id_t getTunnelId() const
    {
        return ids_.tunnel_id;
    }

    sai_object_id_t getDecapMapId() const
    {
        return ids_.tunnel_decap_id;
    }

    sai_object_id_t getEncapMapId() const
    {
        return ids_.tunnel_encap_id;
    }

    void updateNHTunnel(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni, sai_object_id_t nh_id);
    bool removeNHTunnel(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni);
    sai_object_id_t getNHTunnel(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni) const;

    void incNHTunnelRefCount(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni);
    void decNHTunnelRefCount(IpAddress& ipAddr, MacAddress macAddress, uint32_t vni);

private:
    string tunnel_name_;
    bool active_ = false;

    tunnel_ids_t ids_;
    std::pair<MAP_T, MAP_T> tunnel_map_ = { MAP_T::MAP_TO_INVALID, MAP_T::MAP_TO_INVALID };

    TunnelMapEntries tunnel_map_entries_;
    TunnelNHs nh_tunnels_;

    IpAddress src_ip_;
    IpAddress dst_ip_ = 0x0;
};

const request_description_t vxlan_tunnel_request_description = {
            { REQ_T_STRING },
            {
                { "src_ip", REQ_T_IP },
                { "dst_ip", REQ_T_IP },
            },
            { "src_ip" }
};

class VxlanTunnelRequest : public Request
{
public:
    VxlanTunnelRequest() : Request(vxlan_tunnel_request_description, '|') { }
};

typedef std::unique_ptr<VxlanTunnel> VxlanTunnel_T;
typedef std::map<std::string, VxlanTunnel_T> VxlanTunnelTable;

typedef enum
{
    TUNNEL_MAP_T_VLAN,
    TUNNEL_MAP_T_BRIDGE,
    TUNNEL_MAP_T_VIRTUAL_ROUTER,
} tunnel_map_type_t;

class VxlanTunnelOrch : public Orch2
{
public:
    VxlanTunnelOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isTunnelExists(const std::string& tunnelName) const
    {
        return vxlan_tunnel_table_.find(tunnelName) != std::end(vxlan_tunnel_table_);
    }

    VxlanTunnel* getVxlanTunnel(const std::string& tunnelName)
    {
        return vxlan_tunnel_table_.at(tunnelName).get();
    }

    bool createVxlanTunnelMap(string tunnelName, tunnel_map_type_t mapType, uint32_t vni,
                              sai_object_id_t encap, sai_object_id_t decap);

    bool removeVxlanTunnelMap(string tunnelName, uint32_t vni);

    sai_object_id_t
    createNextHopTunnel(string tunnelName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni=0);

    bool
    removeNextHopTunnel(string tunnelName, IpAddress& ipAddr, MacAddress macAddress, uint32_t vni=0);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VxlanTunnelTable vxlan_tunnel_table_;
    VxlanTunnelRequest request_;
};

const request_description_t vxlan_tunnel_map_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vni",  REQ_T_UINT },
                { "vlan", REQ_T_VLAN },
            },
            { "vni", "vlan" }
};

typedef std::map<std::string, sai_object_id_t> VxlanTunnelMapTable;

class VxlanTunnelMapRequest : public Request
{
public:
    VxlanTunnelMapRequest() : Request(vxlan_tunnel_map_request_description, '|') { }
};

class VxlanTunnelMapOrch : public Orch2
{
public:
    VxlanTunnelMapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isTunnelMapExists(const std::string& name) const
    {
        return vxlan_tunnel_map_table_.find(name) != std::end(vxlan_tunnel_map_table_);
    }
private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VxlanTunnelMapTable vxlan_tunnel_map_table_;
    VxlanTunnelMapRequest request_;
};

const request_description_t vxlan_vrf_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vni", REQ_T_UINT },
                { "vrf", REQ_T_STRING },
            },
            { "vni", "vrf" }
};

class VxlanVrfRequest : public Request
{
public:
    VxlanVrfRequest() : Request(vxlan_vrf_request_description, ':') { }
};

struct vrf_map_entry_t {
    sai_object_id_t encap_id;
    sai_object_id_t decap_id;
};

typedef std::map<string, vrf_map_entry_t> VxlanVrfTable;
typedef std::map<string, sai_object_id_t> VxlanVrfTunnel;

class VxlanVrfMapOrch : public Orch2
{
public:
    VxlanVrfMapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    typedef std::pair<sai_object_id_t, sai_object_id_t> handler_pair;

    bool isVrfMapExists(const std::string& name) const
    {
        return vxlan_vrf_table_.find(name) != std::end(vxlan_vrf_table_);
    }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VxlanVrfTable vxlan_vrf_table_;
    VxlanVrfTunnel vxlan_vrf_tunnel_;
    VxlanVrfRequest request_;
};
