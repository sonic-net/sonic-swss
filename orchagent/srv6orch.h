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
    string            endAdjString; // Used for END.X, END.DX4, END.DX6
};

struct Srv6TunnelMapEntryKey
{
    string endpoint;
    string vpn_sid;
    uint32_t prefix_agg_id;

    bool operator==(const Srv6TunnelMapEntryKey &o) const
    {
        return tie(endpoint, vpn_sid, prefix_agg_id) ==
            tie(o.endpoint, o.vpn_sid, o.prefix_agg_id);
    }

    bool operator<(const Srv6TunnelMapEntryKey &o) const
    {
        return tie(endpoint, vpn_sid, prefix_agg_id) <
            tie(o.endpoint, o.vpn_sid, o.prefix_agg_id);
    }

    bool operator!=(const Srv6TunnelMapEntryKey &o) const
    {
        return !(*this == o);
    }
};

struct Srv6TunnelMapEntryEntry
{
    sai_object_id_t tunnel_map_entry_id;

    // for sid remarking
    sai_object_id_t inner_tunnel_map_id;
    map<uint8_t, sai_object_id_t> inner_tunnel_map_entry_ids; 

    uint32_t ref_count;
};

struct P2pTunnelEntry
{
    sai_object_id_t tunnel_id;
    sai_object_id_t tunnel_map_id;

    set<NextHopKey> nexthops;
    set<Srv6TunnelMapEntryKey> tunnel_map_entries;
};

struct Srv6PrefixAggIdEntry
{
    uint32_t prefix_agg_id;

    uint32_t ref_count;
};

struct Srv6PicContextInfo
{
    vector<string> nexthops;
    vector<string> sids;
    uint32_t ref_count;
};

typedef unordered_map<string, SidTableEntry> SidTable;
typedef unordered_map<string, SidTunnelEntry> Srv6TunnelTable;
typedef map<NextHopKey, sai_object_id_t> Srv6NextHopTable;
typedef unordered_map<string, MySidEntry> Srv6MySidTable;
typedef map<string, P2pTunnelEntry> Srv6P2pTunnelTable;
typedef map<NextHopGroupKey, Srv6PrefixAggIdEntry> Srv6PrefixAggIdTable;
typedef map<string, Srv6PrefixAggIdEntry> Srv6PrefixAggIdTableForNhg;
typedef set<uint32_t> Srv6PrefixAggIdSet;
typedef map<Srv6TunnelMapEntryKey, Srv6TunnelMapEntryEntry> Srv6TunnelMapEntryTable;
typedef map<string, Srv6PicContextInfo> Srv6PicContextTable;

#define SID_LIST_DELIMITER ','
#define MY_SID_KEY_DELIMITER ':'
class Srv6Orch : public Orch, public Observer
{
    public:
        Srv6Orch(DBConnector *applDb, vector<string> &tableNames, SwitchOrch *switchOrch, VRFOrch *vrfOrch, NeighOrch *neighOrch):
          Orch(applDb, tableNames),
          m_vrfOrch(vrfOrch),
          m_switchOrch(switchOrch),
          m_neighOrch(neighOrch),
          m_sidTable(applDb, APP_SRV6_SID_LIST_TABLE_NAME),
          m_mysidTable(applDb, APP_SRV6_MY_SID_TABLE_NAME),
          m_piccontextTable(applDb, APP_PIC_CONTEXT_TABLE_NAME)
        {
            m_neighOrch->attach(this);
        }
        ~Srv6Orch()
        {
            m_neighOrch->detach(this);
        }
        void increasePicContextIdRefCount(const std::string&);
        void decreasePicContextIdRefCount(const std::string&);
        void increasePrefixAggIdRefCount(const NextHopGroupKey&);
        void increasePrefixAggIdRefCount(const std::string&);
        void decreasePrefixAggIdRefCount(const NextHopGroupKey&);
        void decreasePrefixAggIdRefCount(const std::string&);
        uint32_t getAggId(const NextHopGroupKey &nhg);
        uint32_t getAggId(const std::string& index);
        void deleteAggId(const NextHopGroupKey &nhg);
        void deleteAggId(const std::string& index);
        bool createSrv6NexthopWithoutVpn(const NextHopKey &nhKey, sai_object_id_t &nexthop_id);
        bool srv6Nexthops(const NextHopGroupKey &nextHops, sai_object_id_t &next_hop_id);
        bool removeSrv6NexthopWithoutVpn(const NextHopKey &nhKey);
        bool removeSrv6Nexthops(const std::vector<NextHopGroupKey> &nhgv);
        void update(SubjectType, void *);
        bool contextIdExists(const std::string &context_id);

    private:
        void doTask(Consumer &consumer);
        void doTaskSidTable(const KeyOpFieldsValuesTuple &tuple);
        void doTaskMySidTable(const KeyOpFieldsValuesTuple &tuple);
        task_process_status doTaskPicContextTable(const KeyOpFieldsValuesTuple &tuple);
        bool createUpdateSidList(const string seg_name, const string ips, const string sidlist_type);
        bool deleteSidList(const string seg_name);
        bool createSrv6Tunnel(const string srv6_source);
        bool createSrv6Nexthop(const NextHopKey &nh);
        bool deleteSrv6Nexthop(const NextHopKey &nh);
        bool srv6NexthopExists(const NextHopKey &nh);
        bool createUpdateMysidEntry(string my_sid_string, const string vrf, const string adj, const string end_action);
        bool deleteMysidEntry(const string my_sid_string);
        bool sidEntryEndpointBehavior(const string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                      sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor);
        bool mySidExists(const string mysid_string);
        bool mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior);
        bool mySidNextHopRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior);
        void srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert);
        size_t srv6TunnelNexthopSize(const string srv6_source);
        bool srv6P2pTunnelExists(const string &endpoint);
        bool createSrv6P2pTunnel(const string &src, const string &endpoint);
        bool deleteSrv6P2pTunnel(const string &endpoint);
        void srv6P2ptunnelUpdateNexthops(const NextHopKey &nhkey, bool insert);
        size_t srv6P2pTunnelNexthopSize(const string &endpoint);
        void srv6P2pTunnelUpdateEntries(const Srv6TunnelMapEntryKey &tmek, bool insert);
        size_t srv6P2pTunnelEntrySize(const string &endpoint);

        bool createSrv6Vpn(const string &endpoint, const string &sid, const uint32_t prefix_agg_id);
        bool createSrv6Vpns(const Srv6PicContextInfo &pci ,const std::string &context_id);
        bool deleteSrv6Vpn(const string &endpoint, const string &sid, const uint32_t prefix_agg_id);
        bool deleteSrv6Vpns(const std::string &context_id);
        void updateNeighbor(const NeighborUpdate& update);

        ProducerStateTable m_sidTable;
        ProducerStateTable m_mysidTable;
        ProducerStateTable m_piccontextTable;
        SidTable sid_table_;
        Srv6TunnelTable srv6_tunnel_table_;
        Srv6NextHopTable srv6_nexthop_table_;
        Srv6MySidTable srv6_my_sid_table_;
        Srv6P2pTunnelTable srv6_p2p_tunnel_table_;
        Srv6PrefixAggIdTable srv6_prefix_agg_id_table_;
        Srv6PrefixAggIdTableForNhg srv6_prefix_agg_id_table_for_nhg_;
        Srv6PrefixAggIdSet srv6_prefix_agg_id_set_;
        Srv6TunnelMapEntryTable srv6_tunnel_map_entry_table_;
        Srv6PicContextTable srv6_pic_context_table_;

        VRFOrch *m_vrfOrch;
        SwitchOrch *m_switchOrch;
        NeighOrch *m_neighOrch;

        /*
         * Map to store the SRv6 MySID entries not yet configured in ASIC because associated to a non-ready nexthop
         * 
         *    Key: nexthop
         *    Value: a set of SID entries that are waiting for the nexthop to be ready
         *           each SID entry is encoded as a tuple <My SID key, VRF name, Adjacency, SRv6 Behavior>
         */
        map<NextHopKey, set<tuple<string, string, string, string>>> m_pendingSRv6MySIDEntries;
};

#endif // SWSS_SRV6ORCH_H
