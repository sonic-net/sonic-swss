#ifndef SWSS_TUNNELDECAPORCH_H
#define SWSS_TUNNELDECAPORCH_H

#include <arpa/inet.h>
#include <unordered_set>

#include "orch.h"
#include "sai.h"
#include "ipaddress.h"
#include "ipaddresses.h"

struct TunnelTermEntry
{
    sai_object_id_t                 tunnel_term_id;
    std::string                     ip_address;
};

struct TunnelEntry
{
    sai_object_id_t                 tunnel_id;              // tunnel id
    sai_object_id_t                 overlay_intf_id;        // overlay interface id
    std::vector<TunnelTermEntry>    tunnel_term_info;       // tunnel_entry ids related to the tunnel abd ips related to the tunnel (all ips for tunnel entries that refer to this tunnel)
};

/* TunnelTable: key string, tunnel object id */
typedef std::map<std::string, TunnelEntry> TunnelTable;

/* ExistingIps: ips that currently have term entries */
typedef std::unordered_set<std::string> ExistingIps;

class TunnelDecapOrch : public Orch
{
public:
    TunnelDecapOrch(swss::DBConnector *db, std::string tableName);

private:
    TunnelTable tunnelTable;
    ExistingIps existingIps;

    bool addDecapTunnel(std::string key, std::string type, swss::IpAddresses dst_ip, swss::IpAddress* p_src_ip, std::string dscp, std::string ecn, std::string ttl);
    bool removeDecapTunnel(std::string key);

    bool addDecapTunnelTermEntries(std::string tunnelKey, swss::IpAddresses dst_ip, sai_object_id_t tunnel_id);
    bool removeDecapTunnelTermEntry(sai_object_id_t tunnel_term_id, std::string ip);

    bool setTunnelAttribute(std::string field, std::string value, sai_object_id_t existing_tunnel_id);
    bool setIpAttribute(std::string key, swss::IpAddresses new_ip_addresses, sai_object_id_t tunnel_id);

    void doTask(Consumer& consumer);
};
#endif
