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
    bool ecmp;
    NextHopKey nhkey; // Used for END.X, END.DX4/6, END.B6.Encaps
    NextHopGroupKey nhgkey; // Used for END.X, END.DX4/6, END.B6.Encaps
};

typedef unordered_map<string, SidTableEntry> SidTable;
typedef unordered_map<string, SidTunnelEntry> Srv6TunnelTable;
typedef map<NextHopKey, sai_object_id_t> Srv6NextHopTable;
typedef unordered_map<string, MySidEntry> Srv6MySidTable;
typedef map<NextHopKey, set<string>> Srv6MySidNexthopTable;
typedef map<NextHopGroupKey, set<string>> Srv6MySidNexthopGroupTable;

#define SID_LIST_DELIMITER ','
#define MY_SID_KEY_DELIMITER ':'
class Srv6Orch : public Orch, public Observer, public Subject
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

        }
        bool srv6Nexthops(const NextHopGroupKey &nextHops, sai_object_id_t &next_hop_id);
        bool removeSrv6Nexthops(const NextHopGroupKey &nhg);
        void update(SubjectType, void *);

    private:
        void doTask(Consumer &consumer);
        void doTaskSidTable(const KeyOpFieldsValuesTuple &tuple);
        void doTaskMySidTable(const KeyOpFieldsValuesTuple &tuple);
        bool createUpdateSidList(const string seg_name, const string ips);
        bool deleteSidList(const string seg_name);
        bool createSrv6Tunnel(const string srv6_source);
        bool createSrv6Nexthop(const NextHopKey &nh);
        bool srv6NexthopExists(const NextHopKey &nh);
        bool createUpdateMysidEntry(string my_sid_string, const string vrf, const string end_action, const string nexthop);
        bool deleteMysidEntry(const string my_sid_string);
        bool sidEntryEndpointBehavior(const string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                      sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor);
        bool mySidExists(const string mysid_string);
        bool mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior);
        bool mySidXConnectNexthopRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior);
        bool mySidXConnectNexthop(const string my_sid_string, const string nh, sai_object_id_t &nh_id);
        void mySidNexthopMapUpdate(string my_sid_string, NextHopKey nhkey);
        void mySidNexthopGroupMapUpdate(string my_sid_string, NextHopGroupKey nhgkey);
        bool mySidUpdateNexthop(const string sid_string, bool ecmp, const NextHopKey nhkey, const NextHopGroupKey nhgkey);
        void updateMySidEntries(const NeighborUpdate update);
        void srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert);
        size_t srv6TunnelNexthopSize(const string srv6_source);


        ProducerStateTable m_sidTable;
        ProducerStateTable m_mysidTable;
        SidTable sid_table_;
        Srv6TunnelTable srv6_tunnel_table_;
        Srv6NextHopTable srv6_nexthop_table_;
        Srv6MySidTable srv6_my_sid_table_;
        Srv6MySidNexthopTable srv6_my_sid_nexthop_table_;
        Srv6MySidNexthopGroupTable srv6_my_sid_nexthop_group_table_;
        VRFOrch *m_vrfOrch;
        SwitchOrch *m_switchOrch;
        NeighOrch *m_neighOrch;
};

#endif // SWSS_SRV6ORCH_H
