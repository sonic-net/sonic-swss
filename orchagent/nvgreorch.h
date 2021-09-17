#pragma once

#include "request_parser.h"
#include "orch.h"

const request_description_t nvgre_tunnel_request_description = {
            { REQ_T_STRING },
            {
                { "src_ip", REQ_T_IP },
            },
            { "src_ip" }
};

class NvgreTunnelRequest : public Request
{
public:
    NvgreTunnelRequest() : Request(nvgre_tunnel_request_description, '|') { }
};

class NvgreTunnelOrch : public Orch2
{
public:
    NvgreTunnelOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    NvgreTunnelRequest request_;
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
    NvgreTunnelMapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    NvgreTunnelMapRequest request_;
};