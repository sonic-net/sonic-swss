#pragma once

#include <memory>

#include "sai.h"
#include "orch.h"
#include "request_parser.h"
// without include portorch compile error ???
#include "portsorch.h"

const int MAP_SIZE = 2;

const std::array<uint32_t, MAP_SIZE> nvgreEncapTunnelMap = {
    SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI,
    SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI
};

const std::array<uint32_t, MAP_SIZE> nvgreDecapTunnelMap = {
    SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID,
    SAI_TUNNEL_MAP_TYPE_VNI_TO_BRIDGE_IF
};

struct tunnel_sai_ids_t
{
    sai_object_id_t tunnel_encap_id[MAP_SIZE+1];
    sai_object_id_t tunnel_decap_id[MAP_SIZE+1];
    sai_object_id_t tunnel_id;
};

const request_description_t nvgre_tunnel_request_description = {
            { REQ_T_STRING },
            {
                { "src_ip", REQ_T_IP },
            },
            { "src_ip" }
};

class NvgreTunnel
{
public:
    NvgreTunnel(std::string tunnelName, IpAddress srcIp);
    ~NvgreTunnel(){ }

private:
    void createTunnelMapCapabilities();
    void createTunnel();
    // create structure to hold tunnel_id, encap, decap mappers
    // create methods to add encap and decap mappers tu structure
    // create method to add tunnel to structure
    std::string tunnel_name_;
    IpAddress src_ip_;
    tunnel_sai_ids_t tunnel_ids_ = {{0}, {0}, 0};
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