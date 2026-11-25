#ifndef VRRPORCH_H
#define VRRPORCH_H

#include <string>
#include <sstream>
#include "orch.h"
#include "request_parser.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "schema.h"

extern sai_object_id_t gVirtualRouterId;

struct vrrp_key_t
{
    std::string port_name;
    IpPrefix ip_addr;

    vrrp_key_t() = default;

    vrrp_key_t(std::string portName, IpPrefix ipAddr)
    {
        port_name = portName;
        ip_addr = ipAddr;
    };

    bool operator== (const vrrp_key_t& rhs) const
    {
        if (!(ip_addr == rhs.ip_addr) || port_name != rhs.port_name)
        {
            return false;
        }
        return true;
    }
};

struct vrrp_data_t
{
    MacAddress vmac;
    sai_object_id_t rifid;
};

struct vrrp_key_hash
{
    size_t operator() (const vrrp_key_t& key) const
    {
        stringstream ss;
        ss << key.port_name << key.ip_addr.to_string();
        return std::hash<std::string>() (ss.str());
    }
};

typedef std::unordered_map<vrrp_key_t, vrrp_data_t, vrrp_key_hash> VrrpTable;

const request_description_t request_desc = {
    { REQ_T_STRING, REQ_T_IP_PREFIX },
    {
        { "vmac", REQ_T_MAC_ADDRESS },
    },
    { } // no mandatory attributes
};

class VrrpRequest : public Request
{
public:
    VrrpRequest() : Request(request_desc, '|') { }
};

class VrrpOrch : public Orch2
{
public:
    VrrpOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);
    bool hasSameIpAddr(const string &alias,const IpPrefix &vip_prefix);
    VrrpTable vrrp_table_;
    VrrpRequest request_;
};

#endif  //VRRPORCH_H
