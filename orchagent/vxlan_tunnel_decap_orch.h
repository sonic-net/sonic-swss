#ifndef __VXLAN_TUNNEL_DECAP_H
#define __VXLAN_TUNNEL_DECAP_H

#include <map>
#include "request_parser.h"

typedef std::tuple<IpAddress, uint32_t> tunnel_id_t;
typedef std::map<tunnel_id_t, sai_object_id_t> VxlanTunnelDecapTable;

const request_description_t vxlan_request_description = {
            { REQ_T_IP, REQ_T_UINT },
            {
                { "vlan", REQ_T_VLAN },
            },
            { "vlan" }
};

class VxlanTunnelDecapRequest : public Request
{
public:
    VxlanTunnelDecapRequest() : Request(vxlan_request_description, '|') { }
};


class VxlanTunnelDecapOrch : public Orch2
{
public:
    VxlanTunnelDecapOrch(DBConnector *db, const std::string& tableName) : Orch2(db, tableName, request_) { }

    bool isTunnelExists(const tunnel_id_t& tunnel_id) const
    {
        return vxlan_tunnel_decap_table_.find(tunnel_id) != std::end(vxlan_tunnel_decap_table_);
    }
private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    VxlanTunnelDecapTable vxlan_tunnel_decap_table_;
    VxlanTunnelDecapRequest request_;
};

#endif //__VXLAN_TUNNEL_DECAP_H
