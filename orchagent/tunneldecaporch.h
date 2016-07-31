#ifndef SWSS_TUNNELDECAPORCH_H
#define SWSS_TUNNELDECAPORCH_H

#include <arpa/inet.h>
#include <list>
#include <unordered_set>

#include "orch.h"
#include "sai.h"
#include "ipaddress.h"
#include "ipaddresses.h"

struct TunnelTermEntry
{
    sai_object_id_t            tunnel_term_id;
    string                     ip_address;
};

struct TunnelEntry
{
	sai_object_id_t            tunnel_id;              // tunnel id
	vector<TunnelTermEntry>	   tunnel_term_info;       // tunnel_entry ids related to the tunnel abd ips related to the tunnel (all ips for tunnel entries that refer to this tunnel)
};

/* TunnelTable: key string, tunnel object id */
typedef map<string, TunnelEntry> TunnelTable;

/* ExistingIps: ips that currently have term entries */
typedef unordered_set<string> ExistingIps;

class TunnelDecapOrch : public Orch
{
public:
    TunnelDecapOrch(DBConnector *db, string tableName);

private:

	TunnelTable tunnelTable;
	ExistingIps existingIps;

    bool addDecapTunnel(string key, string type, IpAddresses dst_ip, string dscp, string ecn, string ttl);
    bool removeDecapTunnel(string key);

    bool addDecapTunnelTermEntries(string tunnelKey, IpAddresses dst_ip, sai_object_id_t tunnel_id);
    bool removeDecapTunnelTermEntry(string key, TunnelTermEntry &tunnel_term_info);

    bool setTunnelAttribute(string field, string value, sai_object_id_t existing_tunnel_id);
    bool setIpAttribute(string key, IpAddresses new_ip_addresses, sai_object_id_t tunnel_id);

    void doTask(Consumer& consumer);
};
#endif
