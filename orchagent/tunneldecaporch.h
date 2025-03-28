#ifndef SWSS_TUNNELDECAPORCH_H
#define SWSS_TUNNELDECAPORCH_H

#include <arpa/inet.h>
#include <unordered_set>

#include "orchagent/orch.h"
#include "sai.h"
#include "ipaddress.h"
#include "ipaddresses.h"


enum TunnelTermType
{
    TUNNEL_TERM_TYPE_P2P,
    TUNNEL_TERM_TYPE_P2MP,
    TUNNEL_TERM_TYPE_MP2MP
};

/* Constants */
#define MUX_TUNNEL "MuxTunnel0"


struct TunnelTermEntry
{
    sai_object_id_t                 tunnel_term_id;
    std::string                     src_ip;
    std::string                     dst_ip;
    TunnelTermType                  term_type;
    std::string                     subnet_type;
};

struct TunnelEntry
{
    sai_object_id_t                             tunnel_id;                  // tunnel id
    sai_object_id_t                             overlay_intf_id;            // overlay interface id
    int                                         ref_count;                  // reference count
    std::map<swss::IpPrefix, TunnelTermEntry>   tunnel_term_info;           // decap terms
    std::string                                 tunnel_type;                // tunnel type, IPINIP only
    std::string                                 dscp_mode;                  // dscp_mode, will be used in muxorch
    std::string                                 ecn_mode;                   // ECN mode
    std::string                                 encap_ecn_mode;             // encap ECN mode
    std::string                                 ttl_mode;                   // TTL mode
    sai_object_id_t                             encap_tc_to_dscp_map_id;    // TC_TO_DSCP map id, will be used in muxorch
    sai_object_id_t                             encap_tc_to_queue_map_id;   // TC_TO_QUEUE map id, will be used in muxorch
};

struct SubnetDecapConfig
{
    bool            enable;
    std::string     src_ip;
    std::string     src_ip_v6;
    std::string     tunnel;
    std::string     tunnel_v6;
};

struct NexthopTunnel
{
    sai_object_id_t nh_id;
    int             ref_count;
};

/* TunnelTable: key string, tunnel object id */
typedef std::map<std::string, TunnelEntry> TunnelTable;

/* Nexthop IP to refcount map */
typedef std::map<swss::IpAddress, NexthopTunnel> Nexthop;

/* Tunnel to nexthop maps */
typedef std::map<std::string, Nexthop> TunnelNhs;

/* unhandled decap term table */
typedef std::map<std::string, std::map<swss::IpPrefix, TunnelTermEntry>> UnhandledDecapTermTable;

class TunnelDecapOrch : public Orch
{
public:
    TunnelDecapOrch(swss::DBConnector *appDb, swss::DBConnector *stateDb,
                    swss::DBConnector *configDb, const std::vector<std::string> &tableNames);

    sai_object_id_t createNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr);
    bool removeNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr);
    swss::IpAddresses getDstIpAddresses(std::string tunnelKey);
    std::string getDscpMode(const std::string &tunnelKey) const;
    bool getQosMapId(const std::string &tunnelKey, const std::string &qos_table_type, sai_object_id_t &oid) const;
    const SubnetDecapConfig &getSubnetDecapConfig() const
    {
        return subnetDecapConfig;
    }

private:
    TunnelTable tunnelTable;
    TunnelNhs   tunnelNhs;
    UnhandledDecapTermTable unhandledDecapTerms;
    std::unique_ptr<swss::Table> stateTunnelDecapTable = nullptr;
    std::unique_ptr<swss::Table> stateTunnelDecapTermTable = nullptr;
    SubnetDecapConfig subnetDecapConfig = {
        false,
        "",
        "",
        "IPINIP_SUBNET",
        "IPINIP_SUBNET_V6"
    };

    bool addDecapTunnel(std::string key, std::string type, swss::IpAddress* p_src_ip,
                        std::string dscp, std::string ecn, std::string encap_ecn, std::string ttl,
                        sai_object_id_t dscp_to_tc_map_id, sai_object_id_t tc_to_pg_map_id);
    bool removeDecapTunnel(std::string table_name, std::string key);

    bool addDecapTunnelTermEntry(std::string tunnel_name, std::string src_ip_str,
                                 std::string dst_ip_str, TunnelTermType term_type, std::string subnet_type);
    bool removeDecapTunnelTermEntry(std::string tunnel_name, std::string dst_ip_str);

    void addUnhandledDecapTunnelTerm(const std::string &tunnel_name, const std::string &src_ip_str,
                                     const std::string &dst_ip_str, TunnelTermType term_type,
                                     const std::string &subnet_type)
    {
        swss::IpPrefix dst_ip(dst_ip_str);
        unhandledDecapTerms[tunnel_name][dst_ip] = {SAI_NULL_OBJECT_ID, src_ip_str, dst_ip_str, term_type, subnet_type};
    }
    void removeUnhandledDecapTunnelTerm(const std::string &tunnel_name, const std::string &dst_ip_str)
    {
        swss::IpPrefix dst_ip(dst_ip_str);
        auto tunnel_it = unhandledDecapTerms.find(tunnel_name);
        if (tunnel_it != unhandledDecapTerms.end())
        {
            tunnel_it->second.erase(dst_ip);
        }
    }
    void updateUnhandledDecapTunnelTerms(const std::string &tunnel_name, const std::string &src_ip_str);
    void processUnhandledDecapTunnelTerms(const std::string &tunnel_name);

    bool setTunnelAttribute(std::string field, std::string value, sai_object_id_t existing_tunnel_id);
    bool setTunnelAttribute(std::string field, sai_object_id_t value, sai_object_id_t existing_tunnel_id);
    bool setIpAttribute(std::string tunnel_name, std::string src_ip_str);

    sai_object_id_t getNextHopTunnel(std::string tunnelKey, swss::IpAddress& ipAddr);
    int incNextHopRef(std::string tunnelKey, swss::IpAddress& ipAddr);
    int decNextHopRef(std::string tunnelKey, swss::IpAddress& ipAddr);

    void doTask(Consumer& consumer);
    void doDecapTunnelTask(Consumer &consumer);
    void doDecapTunnelTermTask(Consumer &consumer);
    void doSubnetDecapTask(Consumer &consumer);
    void doSubnetDecapTask(const swss::KeyOpFieldsValuesTuple &tuple);

    void setDecapTunnelStatus(const std::string &tunnel_name);
    void removeDecapTunnelStatus(const std::string &tunnel_name);
    void setDecapTunnelTermStatus(const std::string &tunnel_name, const std::string &dst_ip_str,
                                  const std::string &src_ip_str, TunnelTermType term_type, const std::string &subnet_type);
    void removeDecapTunnelTermStatus(const std::string &tunnel_name, const std::string &dst_ip_str);
    void RemoveTunnelIfNotReferenced(const std::string &tunnel_name);
    int getTunnelRefCount(const std::string &tunnel_name)
    {
        return tunnelTable[tunnel_name].ref_count;
    }
    void increaseTunnelRefCount(const std::string &tunnel_name)
    {
        ++tunnelTable[tunnel_name].ref_count;
    }
    void decreaseTunnelRefCount(const std::string &tunnel_name)
    {
        --tunnelTable[tunnel_name].ref_count;
    }
};
#endif
