#pragma once

#include <memory>

#include "sai.h"
#include "orch.h"
#include "request_parser.h"
// without include portorch compile error ???
#include "portsorch.h"

const int MAP_SIZE = 2;

const std::vector<sai_tunnel_map_type_t> nvgreEncapTunnelMap = {
    SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI,
    SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI
};

const std::vector<sai_tunnel_map_type_t> nvgreDecapTunnelMap = {
    SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID,
    SAI_TUNNEL_MAP_TYPE_VNI_TO_BRIDGE_IF
};

struct tunnel_sai_ids_t
{
    std::vector<sai_object_id_t> tunnel_encap_id;
    std::vector<sai_object_id_t> tunnel_decap_id;
    sai_object_id_t tunnel_id;
};

typedef struct nvgre_tunnel_map_entry_s
{
   sai_object_id_t map_entry_id;
   uint32_t        vlan_id;
   uint32_t        vsid_id;
} nvgre_tunnel_map_entry_t;

const request_description_t nvgre_tunnel_request_description = {
            { REQ_T_STRING },
            {
                { "src_ip", REQ_T_IP },
            },
            { "src_ip" }
};

typedef std::map<std::string, nvgre_tunnel_map_entry_t> NvgreTunnelMapTable;

class NvgreTunnel
{
public:
    NvgreTunnel(std::string tunnelName, IpAddress srcIp);
    ~NvgreTunnel();

private:
    void createTunnelMappers();
    void removeTunnelMappers();

    void createTunnel();
    void removeTunnel();

    sai_object_id_t sai_create_tunnel_map(sai_tunnel_map_type_t sai_tunnel_map_type);
    void sai_remove_tunnel_map(sai_object_id_t tunnel_map_id);

    sai_object_id_t sai_create_tunnel(struct tunnel_sai_ids_t* ids, sai_ip_address_t *src_ip);
    void sai_remove_tunnel(sai_object_id_t tunnel_id);

    std::string tunnel_name_;
    IpAddress src_ip_;
    tunnel_sai_ids_t tunnel_ids_ = {
        { std::vector<sai_object_id_t>(MAP_SIZE, SAI_NULL_OBJECT_ID) },
        { std::vector<sai_object_id_t>(MAP_SIZE, SAI_NULL_OBJECT_ID) },
        SAI_NULL_OBJECT_ID
    };

    bool isTunnelMapExists(const std::string& name) const
    {
        return nvgre_tunnel_map_table_.find(name) != std::end(nvgre_tunnel_map_table_);
    }

    //sai_object_id_t sai_create_tunnel_map_entry(sai_object_id_t tunnel_map_id, sai_uint32_t vsid, sai_uint16_t vlan_id);
    //void sai_remove_tunnel_map_entry(sai_object_id_t obj_id);

    NvgreTunnelMapTable nvgre_tunnel_map_table_;
};

typedef std::map<std::string, std::unique_ptr<NvgreTunnel>> NvgreTunnelTable;

class NvgreTunnelRequest : public Request
{
public:
    NvgreTunnelRequest() : Request(nvgre_tunnel_request_description, '|') { }
};

class NvgreTunnelOrch : public Orch2
{
public:
    NvgreTunnelOrch(DBConnector *db, const std::string& tableName) :
                    Orch2(db, tableName, request_)
    { }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    bool isTunnelExists(const std::string& tunnelName) const
    {
        return nvgre_tunnel_table_.find(tunnelName) != std::end(nvgre_tunnel_table_);
    }

    NvgreTunnel* getNvgreTunnel(const std::string& tunnelName)
    {
        return nvgre_tunnel_table_.at(tunnelName).get();
    }

    NvgreTunnelRequest request_;
    NvgreTunnelTable nvgre_tunnel_table_;
};

const request_description_t nvgre_tunnel_map_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vsid",  REQ_T_UINT },
                { "vlan", REQ_T_VLAN },
            },
            { "vsid", "vlan" }
};

class NvgreTunnelMapRequest : public Request
{
public:
    NvgreTunnelMapRequest() : Request(nvgre_tunnel_map_request_description, '|') { }
};

class NvgreTunnelMapOrch : public Orch2
{
public:
    NvgreTunnelMapOrch(DBConnector *db, const std::string& tableName) :
                       Orch2(db, tableName, request_)
    {}

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    NvgreTunnelMapRequest request_;
};