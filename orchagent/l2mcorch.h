#ifndef SWSS_L2MCORCH_H
#define SWSS_L2MCORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "debugdumporch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "tokenize.h"

#include <map>
#include <string>
#include <set>

struct L2mcGroupKey
{
    std::string  vlan_alias;
    IpAddress source_address;
    IpAddress group_address;

    L2mcGroupKey() = default;

    L2mcGroupKey(std::string vlan, IpAddress srcAddress, IpAddress grpAddress)
    {
        vlan_alias = vlan;
        source_address = srcAddress;
        group_address = grpAddress;
    };

    bool operator<(const L2mcGroupKey& o) const
    {
        return tie(vlan_alias, source_address, group_address) < tie(o.vlan_alias, o.source_address, o.group_address);
    }

    bool operator==(const L2mcGroupKey& o) const
    {
        return tie(vlan_alias, source_address, group_address) == tie(o.vlan_alias, o.source_address, o.group_address);
    }
};

typedef std::map<string, sai_object_id_t> L2mcGroupMembers;

struct L2mcGroupEntry
{
    sai_object_id_t         l2mc_group_id;          // l2mc group id
    int                     ref_count;              // group members reference count
    L2mcGroupMembers        l2mc_group_members;     // ids of members indexed by <port_alias, member_obj_id>
};

/* L2mcEntryTable: L2mcGroupKey, L2mcGroupEntry */
typedef std::map<L2mcGroupKey, L2mcGroupEntry> L2mcEntryTable;
/*L2mcEntryTables: Vlan, L2mcEntryTable */
typedef std::map<string, L2mcEntryTable> L2mcEntryTables;
/*mrouter_ports: Vlan, mrouter_ports */
typedef unordered_map<string, vector<string>> mrouter_ports;

struct L2mcDebugCounters
{
    int l2mc_entry_add;
    int l2mc_entry_del;
    int l2mc_group_add;
    int l2mc_group_del;
    int l2mc_member_add;
    int l2mc_member_del;
    int l2mc_member_add_fail;
    int l2mc_member_del_fail;
    int l2mc_group_add_fail;
    int l2mc_group_del_fail;
    int l2mc_entry_add_fail;
    int l2mc_entry_del_fail;
    int l2mc_mrouter_add;
    int l2mc_mrouter_del;
    int l2mc_mrouter_add_fail;
    int l2mc_mrouter_del_fail;
    int l2mc_vlan_fail;
    int l2mc_port_fail;
    int l2mc_vlan_oid_fail;
    int l2mc_port_oid_fail;
};

class L2mcOrch : public Orch, public DebugDump
{
public:
    L2mcOrch(DBConnector *appDb, vector<string> &tableNames);

    ~L2mcOrch()
    {
        // do nothing
    }

    bool hasL2mcGroup(string vlan, const L2mcGroupKey&);
    sai_object_id_t getL2mcGroupId(string vlan, const L2mcGroupKey&);
    
    void increaseL2mcMemberRefCount(string vlan, const L2mcGroupKey&);
    void decreaseL2mcMemberRefCount(string vlan, const L2mcGroupKey&);
    bool isMemberRefCntZero(string vlan, const L2mcGroupKey&) const;
    bool debugdumpCLI(KeyOpFieldsValuesTuple  t);
    void l2mcDbgDumpAll();

private:

    L2mcEntryTables m_syncdL2mcEntries;
    mrouter_ports mrouter_ports_per_vlan;
    vector<string> m_snoop_enabled_vlans;

    struct L2mcDebugCounters l2mcdbg_counters; 

    void doTask(Consumer &consumer);
    void doL2mcTask(Consumer &consumer);
    void doL2mcMemberTask(Consumer &consumer);
    void doL2mcMrouterTask(Consumer &consumer);

    bool EnableIgmpSnooping(string vlan);
    bool DisableIgmpSnooping(string vlan);

    bool AddL2mcEntry(const L2mcGroupKey &l2mckey, Port &vlan, Port &port);
    bool RemoveL2mcEntry(const L2mcGroupKey &l2mkey, Port &vlan);
    bool AddL2mcGroupMember(const L2mcGroupKey &l2mc_grpKey, string vlan, Port &port);
    bool RemoveL2mcGroupMember(const L2mcGroupKey &l2mc_grpKey, string vlan, Port &port);

    bool AddL2mcMrouterPort(const L2mcGroupKey &l2mc_grpKey, string vlan, Port &port);
    bool RemoveL2mcMrouterPort(const L2mcGroupKey &l2mc_grpKey, string vlan, string port);
    void addMrouterPortToL2mcEntries(string vlan, Port &port);
    void removeMrouterPortFromL2mcEntries(string vlan, string mrouterport);

    void clearAllL2mcDbgCounters();
    void debugdump_l2mcGrps2Mbrs(string vlan, L2mcGroupMembers &l2mcgrpMembers);
    void debugdump_l2mcEntries(string vlan, bool val);
    void debugdump_l2mcMrouterPorts(string vlan);
    void debugShowSnoopVlans();
    void debugdump_l2mcDbgCounters();
    string gL2mcOrchDbgComp;
};

#endif /* SWSS_L2MCORCH_H */
