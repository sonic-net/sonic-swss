#ifndef SWSS_SRV6ORCH_H
#define SWSS_SRV6ORCH_H

#include <vector>
#include <string>
#include <set>
#include <unordered_map>

#include "dbconnector.h"
#include "orch.h"
#include "observer.h"
#include "switchorch.h"
#include "portsorch.h"
#include "vrforch.h"
#include "redisapi.h"
#include "intfsorch.h"
#include "nexthopgroupkey.h"
#include "nexthopkey.h"
#include "neighorch.h"
#include "producerstatetable.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"

using namespace std;
using namespace swss;

struct SidTableEntry
{
    sai_object_id_t sid_object_id;         // SRV6 SID list object id
    set<NextHopKey> nexthops;              // number of nexthops referencing the object
};

struct SidTunnelEntry
{
    sai_object_id_t tunnel_object_id; // SRV6 tunnel object id
    set<NextHopKey> nexthops;  // SRV6 Nexthops using the tunnel object.
};

struct MySidEntry
{
    sai_my_sid_entry_t entry;
    sai_my_sid_entry_endpoint_behavior_t endBehavior;
    string            endVrfString; // Used for END.T, END.DT4, END.DT6 and END.DT46,
};

struct P2pTunnelKey
{
    string src_ip;
    string endpoint;

    bool operator==(const P2pTunnelKey &o) const
    {
        return tie(src_ip, endpoint) ==
            tie(o.src_ip, o.endpoint);
    }

    bool operator<(const P2pTunnelKey &o) const
    {
        return tie(src_ip, endpoint) <
            tie(o.src_ip, o.endpoint);
    }

    bool operator!=(const P2pTunnelKey &o) const
    {
        return !(*this == o);
    }
};

struct P2pTunnelEntry
{
    sai_object_id_t tunnel_id;
    sai_object_id_t tunnel_map_id;

    set<NextHopKey> nexthops;
};

struct Srv6TunnelMapEntryKey
{
    string src_ip;
    string endpoint;
    string vpn_sid;
    uint32_t prefix_agg_id;

    bool operator==(const Srv6TunnelMapEntryKey &o) const
    {
        return tie(src_ip, endpoint, vpn_sid, prefix_agg_id) ==
            tie(o.src_ip, o.endpoint, o.vpn_sid, o.prefix_agg_id);
    }

    bool operator<(const Srv6TunnelMapEntryKey &o) const
    {
        return tie(src_ip, endpoint, vpn_sid, prefix_agg_id) <
            tie(o.src_ip, o.endpoint, o.vpn_sid, o.prefix_agg_id);
    }

    bool operator!=(const Srv6TunnelMapEntryKey &o) const
    {
        return !(*this == o);
    }
};

struct Srv6TunnelMapEntryEntry
{
    sai_object_id_t tunnel_map_entry_id;

    uint32_t ref_count;
};

struct Srv6PrefixAggIdEntry
{
    uint32_t prefix_agg_id;

    uint32_t ref_count;
};

typedef unordered_map<string, SidTableEntry> SidTable;
typedef unordered_map<string, SidTunnelEntry> Srv6TunnelTable;
typedef map<NextHopKey, sai_object_id_t> Srv6NextHopTable;
typedef unordered_map<string, MySidEntry> Srv6MySidTable;
typedef map<P2pTunnelKey, P2pTunnelEntry> Srv6P2pTunnelTable;
typedef unordered_map<string, Srv6PrefixAggIdEntry> Srv6PrefixAggIdTable;
typedef set<uint32_t> Srv6PrefixAggIdSet;
typedef map<Srv6TunnelMapEntryKey, Srv6TunnelMapEntryEntry> Srv6TunnelMapEntryTable;

#define SID_LIST_DELIMITER ','
#define MY_SID_KEY_DELIMITER ':'
class Srv6Orch : public Orch
{
    public:
        Srv6Orch(DBConnector *applDb, vector<string> &tableNames, SwitchOrch *switchOrch, VRFOrch *vrfOrch, NeighOrch *neighOrch):
          Orch(applDb, tableNames),
          m_vrfOrch(vrfOrch),
          m_switchOrch(switchOrch),
          m_neighOrch(neighOrch),
          m_sidTable(applDb, APP_SRV6_SID_LIST_TABLE_NAME),
          m_mysidTable(applDb, APP_SRV6_MY_SID_TABLE_NAME),
          m_srv6_agg_id(1)
        {
        }
        ~Srv6Orch()
        {

        }

        void increasePrefixAggIdRefCount(const NextHopGroupKey&);
        void decreasePrefixAggIdRefCount(const NextHopGroupKey&);

        uint32_t getAggId(const NextHopGroupKey &nhg);
        void deleteAggId(const NextHopGroupKey &nhg);

        bool srv6Nexthops(const NextHopGroupKey &nextHops, sai_object_id_t &next_hop_id);
        bool removeSrv6Nexthops(const std::vector<NextHopGroupKey> &nhgv);
        bool removeSrv6Nexthops(const NextHopGroupKey &nhg);
        void update(SubjectType, void *);

    private:
        void doTask(Consumer &consumer);
        void doTaskSidTable(const KeyOpFieldsValuesTuple &tuple);
        void doTaskMySidTable(const KeyOpFieldsValuesTuple &tuple);
        bool createUpdateSidList(const string seg_name, const string ips, const string sidlist_type);
        bool deleteSidList(const string seg_name);
        bool createSrv6Tunnel(const string srv6_source);
        bool createSrv6Nexthop(const NextHopKey &nh);
        bool deleteSrv6Nexthop(const NextHopKey &nh);
        bool srv6NexthopExists(const NextHopKey &nh);
        bool createUpdateMysidEntry(string my_sid_string, const string dt_vrf, const string end_action);
        bool deleteMysidEntry(const string my_sid_string);
        bool sidEntryEndpointBehavior(const string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                      sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor);
        bool mySidExists(const string mysid_string);
        bool mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior);
        void srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert);
        size_t srv6TunnelNexthopSize(const string srv6_source);

        bool srv6P2pTunnelExists(const P2pTunnelKey &k);
        bool createSrv6P2pTunnel(const P2pTunnelKey &k);
        bool deleteSrv6P2pTunnel(const P2pTunnelKey &k);
        void srv6P2ptunnelUpdateNexthops(const P2pTunnelKey &k, const NextHopKey &nhkey, bool insert);
        size_t srv6P2pTunnelNexthopSize(const P2pTunnelKey &k);

        bool createSrv6Vpn(const NextHopKey &nh, const uint32_t prefix_agg_id);
        bool deleteSrv6Vpn(const NextHopKey &nh, const uint32_t prefix_agg_id);

        ProducerStateTable m_sidTable;
        ProducerStateTable m_mysidTable;
        SidTable sid_table_;
        Srv6TunnelTable srv6_tunnel_table_;
        Srv6NextHopTable srv6_nexthop_table_;
        Srv6MySidTable srv6_my_sid_table_;
        Srv6P2pTunnelTable srv6_p2p_tunnel_table_;
        Srv6PrefixAggIdTable srv6_prefix_agg_id_table_;
        Srv6PrefixAggIdSet srv6_prefix_agg_id_set_;
        Srv6TunnelMapEntryTable srv6_tunnel_map_entry_table_;
        uint32_t m_srv6_agg_id;
        VRFOrch *m_vrfOrch;
        SwitchOrch *m_switchOrch;
        NeighOrch *m_neighOrch;
};

#endif // SWSS_SRV6ORCH_H
