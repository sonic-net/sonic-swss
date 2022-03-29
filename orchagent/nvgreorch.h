#pragma once

#include <memory>

#include "sai.h"
#include "orch.h"
#include "request_parser.h"
#include "portsorch.h"

typedef enum {
    MAP_T_VLAN = 0,
    MAP_T_BRIDGE = 1,
    MAP_T_MAX = 2
} map_type_t;

struct tunnel_sai_ids_t
{
    std::map<map_type_t, sai_object_id_t> tunnel_encap_id;
    std::map<map_type_t, sai_object_id_t> tunnel_decap_id;
    sai_object_id_t tunnel_id;
    sai_object_id_t tunnel_term_id;
};

typedef struct nvgre_tunnel_map_entry_s
{
   sai_object_id_t map_entry_id;
   sai_vlan_id_t   vlan_id;
   uint32_t        vsid;
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

    bool isTunnelMapExists(const std::string& name) const
    {
        return nvgre_tunnel_map_table_.find(name) != std::end(nvgre_tunnel_map_table_);
    }

    sai_object_id_t getDecapMapId(map_type_t type) const
    {
        return tunnel_ids_.tunnel_decap_id.at(type);
    }

    sai_object_id_t getEncapMapId(map_type_t type) const
    {
        return tunnel_ids_.tunnel_encap_id.at(type);
    }

    sai_object_id_t getMapEntryId(std::string tunnel_map_entry_name)
    {
        return nvgre_tunnel_map_table_.at(tunnel_map_entry_name).map_entry_id;
    }

    sai_object_id_t getMapEntryVlanId(std::string tunnel_map_entry_name)
    {
        return nvgre_tunnel_map_table_.at(tunnel_map_entry_name).vlan_id;
    }

    sai_object_id_t getMapEntryVsid(std::string tunnel_map_entry_name)
    {
        return nvgre_tunnel_map_table_.at(tunnel_map_entry_name).vsid;
    }

    bool addDecapMapperEntry(map_type_t map_type, uint32_t vsid, sai_vlan_id_t vlan_id, std::string tunnel_map_entry_name, sai_object_id_t bridge_obj=SAI_NULL_OBJECT_ID);

    bool delMapperEntry(std::string tunnel_map_entry_name);

private:
    void createNvgreMappers();
    void removeNvgreMappers();

    void createNvgreTunnel();
    void removeNvgreTunnel();

    sai_object_id_t sai_create_tunnel_map(sai_tunnel_map_type_t sai_tunnel_map_type);
    void sai_remove_tunnel_map(sai_object_id_t tunnel_map_id);

    sai_object_id_t sai_create_tunnel(struct tunnel_sai_ids_t &ids, const sai_ip_address_t &src_ip, sai_object_id_t underlay_rif);
    void sai_remove_tunnel(sai_object_id_t tunnel_id);

    sai_object_id_t sai_create_tunnel_termination(sai_object_id_t tunnel_id, const sai_ip_address_t &src_ip, sai_object_id_t default_vrid);
    void sai_remove_tunnel_termination(sai_object_id_t tunnel_term_id);

    sai_object_id_t sai_create_tunnel_map_entry(map_type_t map_type, sai_uint32_t vsid, sai_vlan_id_t vlan_id, sai_object_id_t bridge_obj_id, bool encap=false);
    void sai_remove_tunnel_map_entry(sai_object_id_t obj_id);

    std::string tunnel_name_;
    IpAddress src_ip_;
    tunnel_sai_ids_t tunnel_ids_;

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

    bool isTunnelExists(const std::string& tunnelName) const
    {
        return nvgre_tunnel_table_.find(tunnelName) != std::end(nvgre_tunnel_table_);
    }

    NvgreTunnel* getNvgreTunnel(const std::string& tunnelName)
    {
        return nvgre_tunnel_table_.at(tunnelName).get();
    }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    NvgreTunnelRequest request_;
    NvgreTunnelTable nvgre_tunnel_table_;
};

const request_description_t nvgre_tunnel_map_request_description = {
            { REQ_T_STRING, REQ_T_STRING },
            {
                { "vsid",  REQ_T_UINT },
                { "vlan_id", REQ_T_VLAN },
            },
            { "vsid", "vlan_id" }
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