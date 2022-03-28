#ifndef SWSS_TUNNELDECAPORCH_H
#define SWSS_TUNNELDECAPORCH_H

#include <arpa/inet.h>
#include <unordered_set>

#include "orch.h"
#include "sai.h"
#include "ipaddress.h"
#include "ipaddresses.h"


enum TunnelTermType
{
    TUNNEL_TERM_TYPE_P2P,
    TUNNEL_TERM_TYPE_P2MP
};

/* Constants */
#define MUX_TUNNEL "MuxTunnel0"

#define DECAP_DSCP_TO_DC_MAP    "decap_dscp_to_tc_map"
#define DECAP_TC_TO_PG_MAP      "decap_tc_to_pg_map"

struct TunnelTermEntry
{
    sai_object_id_t                 tunnel_term_id;
    std::string                     src_ip;
    std::string                     dst_ip;
    TunnelTermType                  term_type;
};

struct TunnelEntry
{
    sai_object_id_t                 tunnel_id;              // tunnel id
    sai_object_id_t                 overlay_intf_id;        // overlay interface id
    swss::IpAddresses               dst_ip_addrs;           // destination ip addresses
    std::vector<TunnelTermEntry>    tunnel_term_info;       // tunnel_entry ids related to the tunnel abd ips related to the tunnel (all ips for tunnel entries that refer to this tunnel)
};

struct NexthopTunnel
{
    sai_object_id_t nh_id;
    int             ref_count;
};

/* TunnelTable: key string, tunnel object id */
typedef std::map<std::string, TunnelEntry> TunnelTable;

/* 
    ExistingIps: ips that currently have term entries,
    Key in ExistingIps is src_ip-dst_ip
*/
typedef std::unordered_set<std::string> ExistingIps;

/* Nexthop IP to refcount map */
typedef std::map<swss::IpAddress, NexthopTunnel> Nexthop;

/* Tunnel to nexthop maps */
typedef std::map<std::string, Nexthop> TunnelNhs;

class TunnelDecapOrch : public Orch
{
public:
    TunnelDecapOrch(swss::DBConnector *db, std::string tableName);

    sai_object_id_t createNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr);
    bool removeNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr);
    swss::IpAddresses getDstIpAddresses(std::string tunnelKey) const;

private:
    TunnelTable tunnelTable;
    ExistingIps existingIps;
    TunnelNhs   tunnelNhs;

    bool addDecapTunnel(std::string key, std::string type, swss::IpAddresses dst_ip, swss::IpAddress* p_src_ip,
                        std::string dscp, std::string ecn, std::string encap_ecn, std::string ttl, TunnelTermType term_type,
                        sai_object_id_t dscp_to_tc_map_id, sai_object_id_t tc_to_pg_map_id, sai_object_id_t tc_to_dscp_map_id, sai_object_id_t tc_to_queue_map_id);
    bool removeDecapTunnel(std::string key);

    bool addDecapTunnelTermEntries(std::string tunnelKey, swss::IpAddress src_ip, swss::IpAddresses dst_ip, sai_object_id_t tunnel_id, TunnelTermType type);
    bool removeDecapTunnelTermEntry(sai_object_id_t tunnel_term_id, std::string ip);

    bool setTunnelAttribute(std::string field, std::string value, sai_object_id_t existing_tunnel_id);
    bool setIpAttribute(std::string key, swss::IpAddress src_ip, swss::IpAddresses new_ip_addresses, sai_object_id_t tunnel_id, TunnelTermType term_type);

    sai_object_id_t getNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr);
    int incNextHopRef(std::string tunnelKey, swss::IpAddress& ipAddr);
    int decNextHopRef(std::string tunnelKey, swss::IpAddress& ipAddr);

    void doTask(Consumer& consumer);
};
#endif
