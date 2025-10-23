#ifndef SWSS_L2MCORCH_H
#define SWSS_L2MCORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"


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
struct l2mc_member_t {
    sai_object_id_t l2mc_member_id;
    bool is_l2mc_member = false;
};

typedef std::map<string, l2mc_member_t> L2mcGroupMembers;

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

enum class L2mcSuppressType {
    UNKNOWN_IPV4,
    LINK_LOCAL
};

struct SuppressionGroupInfo 
{
    sai_object_id_t group_id;
    bool unknown_ipv4_enabled = false;
    bool link_local_enabled = false;
};

class L2mcOrch : public Orch, public Observer
{
public:
    L2mcOrch(DBConnector *appDb, vector<string> &tableNames);

    ~L2mcOrch()
    {
        // do nothing
    }

    bool hasL2mcGroup(string vlan, const L2mcGroupKey&);
    sai_object_id_t getL2mcGroupId(string vlan, const L2mcGroupKey&);
    
    bool bake() override;
    void increaseL2mcMemberRefCount(string vlan, const L2mcGroupKey&);
    void decreaseL2mcMemberRefCount(string vlan, const L2mcGroupKey&);
    bool isMemberRefCntZero(string vlan, const L2mcGroupKey&) const;
    bool removeL2mcFromVlan(string vlan);
    void update(SubjectType, void *);
    bool RemoveL2mcEntrys(Port &vlan);
    bool RemoveL2mcGroupMembers(const L2mcGroupKey&, string vlan);
    bool isSuppressionEnabled(string vlan_alias) const;
    void removeMrouterPortFromL2mcEntries(string vlan, string mrouterport);

private:

    L2mcEntryTables m_syncdL2mcEntries;
    mrouter_ports mrouter_ports_per_vlan;
    vector<string> m_snoop_enabled_vlans;
    vector<string> m_pend_snoop_enabled_vlans;
    sai_object_id_t  m_pend_l2mc_group_id = SAI_NULL_OBJECT_ID;  
    std::unordered_map<std::string, SuppressionGroupInfo> m_vlan_suppress_info;


    void doTask(Consumer &consumer);
    void doL2mcTask(Consumer &consumer);
    void doL2mcMemberTask(Consumer &consumer);
    void doL2mcMrouterTask(Consumer &consumer);
    void doL2mcSuppressTask(Consumer &consumer);

    bool EnableIgmpSnooping(string vlan);
    bool DisableIgmpSnooping(string vlan);

    bool AddL2mcEntry(const L2mcGroupKey &l2mckey, Port &vlan, Port &port);
    bool RemoveL2mcEntry(const L2mcGroupKey &l2mkey, Port &vlan);
    bool AddL2mcGroupMember(const L2mcGroupKey &l2mc_grpKey, string vlan, Port &port);
    bool RemoveL2mcGroupMember(const L2mcGroupKey &l2mc_grpKey, string vlan, Port &port);

    bool AddL2mcMrouterPort(const L2mcGroupKey &l2mc_grpKey, string vlan, Port &port);
    bool RemoveL2mcMrouterPort(const L2mcGroupKey &l2mc_grpKey, string vlan, string port);
    bool addMrouterPortToL2mcEntries(string vlan, Port &port);
    //void removeMrouterPortFromL2mcEntries(string vlan, string mrouterport);
    bool dealOptimisedMuliticastFlood(string vlan,bool flag);
    bool dealLinkLocalGroupsSuppress(string vlan,bool flag);
    bool handleL2mcSuppression(const string& vlan_alias, bool enable, L2mcSuppressType type);

};

#endif /* SWSS_L2MCORCH_H */
