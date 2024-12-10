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

typedef unordered_map<string, SidTableEntry> SidTable;
typedef unordered_map<string, SidTunnelEntry> Srv6TunnelTable;
typedef map<NextHopKey, sai_object_id_t> Srv6NextHopTable;
typedef unordered_map<string, MySidEntry> Srv6MySidTable;

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
          m_mysidTable(applDb, APP_SRV6_MY_SID_TABLE_NAME)
        {
            m_neighOrch->attach(this);
        }
        ~Srv6Orch()
        {
            m_neighOrch->detach(this);
        }
        bool srv6Nexthops(const NextHopGroupKey &nextHops, sai_object_id_t &next_hop_id);
        bool removeSrv6Nexthops(const NextHopGroupKey &nhg);
        void update(SubjectType, void *);

    private:
        void doTask(Consumer &consumer);
        task_process_status doTaskSidTable(const KeyOpFieldsValuesTuple &tuple);
        void doTaskMySidTable(const KeyOpFieldsValuesTuple &tuple);
        bool createUpdateSidList(const string seg_name, const string ips, const string sidlist_type);
        task_process_status deleteSidList(const string seg_name);
        bool createSrv6Tunnel(const string srv6_source);
        bool createSrv6Nexthop(const NextHopKey &nh);
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

        void updateNeighbor(const NeighborUpdate& update);

        ProducerStateTable m_sidTable;
        ProducerStateTable m_mysidTable;
        SidTable sid_table_;
        Srv6TunnelTable srv6_tunnel_table_;
        Srv6NextHopTable srv6_nexthop_table_;
        Srv6MySidTable srv6_my_sid_table_;
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
