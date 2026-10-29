#ifndef SWSS_L2NHGORCH_H
#define SWSS_L2NHGORCH_H

#include <vector>
#include <set>
#include "macaddress.h"
#include <nhgorch.h>

#include "orch.h"
#include "observer.h"

enum class L2NhgChange
{
    READY,
    WITHDRAWING,
    REMOVED
};

struct L2NhgUpdate
{
    uint32_t nhg_id;
    sai_object_id_t oid;
    L2NhgChange change;
};

// A reservation pins one concrete group incarnation. Keep this handle until
// every partial/committed hardware attachment has been cleared successfully.
struct L2NhgReference
{
    uint32_t nhg_id = 0;
    sai_object_id_t oid = SAI_NULL_OBJECT_ID;
    uint64_t token = 0;
};

class L2NhgOrch : public NhgOrchCommon<NextHopGroup>, public Subject
{
public:
    L2NhgOrch(DBConnector* appDbConnector,string appL2NhgTable);

    ~L2NhgOrch();
    void doTask() override;

    /*
     * Getters.
     */
    string getNextHopGroupPortName(const std::string& nhg_id);
    unsigned long getL2NhgCount();
    unsigned long getNumL2NhgNextHops(const std::string &nhg_id);
    unsigned long getL2NhVtepRefCount(const std::string &nhg_id);
    bool hasActiveL2Nhg(const std::string &nhg_id);
    bool isL2NextHop(const std::string &nhg_id);

    bool getNhgOid(uint32_t nhg_id, sai_object_id_t &oid) const;
    // Counted compatibility API for synchronous callers. Protection attachment
    // should use the identity-safe reservation API below.
    bool refNhg(uint32_t nhg_id);
    void unrefNhg(uint32_t nhg_id);
    bool reserveNhg(uint32_t nhg_id, L2NhgReference &reference);
    bool commitNhgRef(const L2NhgReference &reference);
    void unrefNhg(const L2NhgReference &reference);

private:
    struct ProtectionMember
    {
        uint32_t owner = 0;
        string ip;
        swss::MacAddress mac;
        bool l3 = false;
        bool deleted = false;
        string peer; // Actual resource; desired fields above may already differ.
    };
    struct ProtectionPeer
    {
        string source;
        string tunnel;
        swss::MacAddress mac;
        sai_object_id_t nh = SAI_NULL_OBJECT_ID;
        std::set<uint32_t> owners;
    };
    struct ProtectionGroup
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        std::set<string> desired;
        map<string, sai_object_id_t> members;
        bool deleted = false;
        bool ready = false;
        uint32_t id = 0;
        uint32_t legacyReferences = 0;
        map<uint64_t, bool> references; // token -> committed; false is reserved
        bool announcedReady = false;
        bool withdrawing = false;
    };
    uint64_t m_nextProtectionReference = 0;
    ProtectionGroup *findProtectionReference(const L2NhgReference &reference);
    bool hasProtectionGroupRefs(const ProtectionGroup &group) const;
    bool hasUsableProtectionMember(const ProtectionGroup &group) const;
    bool prepareProtectionGroup(ProtectionGroup &group);
    void finishProtectionGroup(ProtectionGroup &group);
    void notifyProtectionGroup(const ProtectionGroup &group, L2NhgChange change);
    map<string, ProtectionMember> m_protectionMembers;
    map<string, ProtectionPeer> m_protectionPeers;
    map<string, ProtectionGroup> m_protectionGroups;
    bool updateProtectionRow(const KeyOpFieldsValuesTuple &row);
    bool deleteProtectionRow(const string &key);
    bool protectionMemberReady(const string &key) const;
    bool getProtectionRmacTarget(const string &ip, swss::MacAddress &mac) const;
    void reconcileProtectionRmac();
    bool ensureProtectionMember(ProtectionMember &member);
    bool releaseProtectionMember(ProtectionMember &member);
    bool releaseProtectionPeer(const string &ip, ProtectionPeer &peer);
    bool reconcileProtectionGroup(ProtectionGroup &group, bool withdrawOnly = false, bool removeMembers = true);
    void reconcileProtection();
    sai_object_id_t createSaiBridgeNextHop(sai_object_id_t tunnel, const string &ip, bool protection = false);

    vector<Table*> m_appTables;
    Table m_fpmL2Ids;

    struct l2nhg_vtep_info
    {
        string ip;
        int ref_count;
        string source_vtep;
    };
    unordered_map<string, l2nhg_vtep_info> m_nhg_vtep;

    /*
     * Structure to store Next hop related SAI OIDs
     */
    struct NhIds
    {
        sai_object_id_t nhgm_oid;
        sai_object_id_t nh_oid;
    };

    struct l2nhg_nh_info
    {
        map<string, NhIds> next_hops;
        sai_object_id_t oid;
        bool is_active = false; // True only when bridge port exists
        string source_vtep;
    };
    unordered_map<string, l2nhg_nh_info> m_nhg_nh;

    bool deleteL2NextHop(string nhg_id);
    bool deleteL2NextHopGroup(string nhg_id);
    bool addL2NextHopGroupEntry(string nhg_id, string nh_ids, string source_vtep = "");

    bool removeSaiNextHop(NhIds nh_ids);
    pair<sai_object_id_t, sai_object_id_t> createSaiNextHop(sai_object_id_t l2_nhg_id,
                                                            sai_object_id_t tunnel_id,
                                                            const string& remote_vtep_ip);
    bool removeSaiNextHopGroup(sai_object_id_t l2_nhg_id, bool protection = false);
    sai_object_id_t createSaiNextHopGroup(bool protection = false);

    bool deleteL2Nhg(string& key, Consumer& consumer);
    bool updateL2Nhg(string& key, KeyOpFieldsValuesTuple& t, Consumer& consumer);
    bool updateL2NhgVtepIp(string nh_id, string new_vtep_ip);

    void doL2NhgTask(Consumer &consumer);
    void doTask(Consumer &consumer);
};


#endif /* SWSS_L2NHGORCH_H */
