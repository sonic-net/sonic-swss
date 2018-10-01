#pragma once

#include <map>
#include <set>
#include <memory>
#include "request_parser.h"
#include "portsorch.h"
#include "vrforch.h"
#include "vnetorch.h"

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

class VxlanTunnel
{
public:
    VxlanTunnel(string name, IpAddress src_ip, IpAddress dst_ip)
                :tunnel_name(name), src_ip(src_ip), dst_ip(dst_ip) { }

    bool isActive() const
    {
        return active;
    }

    bool createTunnel(MAP_T encap, MAP_T decap);
    sai_object_id_t addEncapMapperEntry(sai_object_id_t obj, uint32_t vni);
    sai_object_id_t addDecapMapperEntry(sai_object_id_t obj, uint32_t vni);

    sai_object_id_t getDecapMapId(const std::string& tunnel_name) const
    {
        return ids.tunnel_decap_id;
    }

    sai_object_id_t getEncapMapId(const std::string& tunnel_name) const
    {
        return ids.tunnel_encap_id;
    }

private:
    string tunnel_name;

    bool active = false;

    tunnel_ids_t ids = {0x0, 0x0, 0x0, 0x0};

    std::pair<MAP_T, MAP_T> tunnel_map = { MAP_T::MAP_TO_INVALID, MAP_T::MAP_TO_INVALID };

    IpAddress       src_ip;
    IpAddress       dst_ip = 0x0;
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

using VxlanTunnel_T = std::unique_ptr<VxlanTunnel>;
typedef std::map<std::string, VxlanTunnel_T> VxlanTunnelTable;

class VxlanTunnelOrch : public Orch2
{
public:
    VxlanTunnelOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isTunnelExists(const std::string& tunnel_name) const
    {
        return vxlan_tunnel_table_.find(tunnel_name) != std::end(vxlan_tunnel_table_);
    }

    VxlanTunnel_T& getVxlanTunnel(const std::string& tunnel_name)
    {
        return vxlan_tunnel_table_.at(tunnel_name);
    }

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

class VxlanVrfMapOrch : public Orch2
{
public:
    VxlanVrfMapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isVrfMapExists(const std::string& name) const
    {
        return vxlan_vrf_table_.find(name) != std::end(vxlan_vrf_table_);
    }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    void doTaskVnet(string& , VxlanTunnel_T& , std::pair<sai_object_id_t, sai_object_id_t>&);

    VxlanVrfTable vxlan_vrf_table_;
    VxlanVrfRequest request_;
};
