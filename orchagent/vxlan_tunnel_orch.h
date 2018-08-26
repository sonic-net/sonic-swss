#pragma once

#include <map>
#include "request_parser.h"
#include "portsorch.h"

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

struct tunnel_ids_t
{
    sai_object_id_t tunnel_map_id;
    sai_object_id_t tunnel_id;
    sai_object_id_t tunnel_term_id;
};
typedef std::map<std::string, tunnel_ids_t> VxlanTunnelTable;

class VxlanTunnelOrch : public Orch2
{
public:
    VxlanTunnelOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isTunnelExists(const std::string& tunnel_name) const
    {
        return vxlan_tunnel_table_.find(tunnel_name) != std::end(vxlan_tunnel_table_);
    }

    sai_object_id_t getTunnelMapId(const std::string& tunnel_name) const
    {
        return vxlan_tunnel_table_.at(tunnel_name).tunnel_map_id;
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
