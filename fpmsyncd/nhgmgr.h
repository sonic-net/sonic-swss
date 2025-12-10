#ifndef NHGMGR_H
#define NHGMGR_H

#include "dbconnector.h"
#include "ipprefix.h"
#include "producerstatetable.h"
#include <nexthopgroup/nexthopgroupfull.h>
#include <nexthopgroup/nexthopgroupfull_json.h>

#include <string.h>

#define NHG_DELIMITER ','
using namespace std;


namespace swss {

    using NextHopGroupFull = fib::NextHopGroupFull;
    using nh_grp_full = fib::nh_grp_full;

    /* Forward Declarations */
    class RIBNHGTable;
    class SonicNHGTable;
    class RIBNHGEntry;
    struct SonicGateWayNHGObject;

    /* There are two types of nexthop key, one is zebra type, another is sonic type, not implemented */
    enum nexthopKeyType {
        NEXTHOP_KEY_TYPE_ZEBRA = 1,
        NEXTHOP_KEY_TYPE_SONIC = 2,
    };

    /* Type of sonic nexthop object */
    enum sonicNhgObjType {
        SONIC_NHG_OBJ_TYPE_NHG_NORMAL = 0,
        SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY = 1,
        SONIC_NHG_OBJ_TYPE_NHG_VXLAN = 2,
    };

    /* Key Object used to hash Sonic Gateway nexthop object */
    struct SonicGateWayNHGObjectKey {
        vector<std::pair<uint32_t, uint32_t>> groupMember;
        string nexthop;
        string vpnSid;
        string segSrc;
        sonicNhgObjType type;

        bool operator<(const SonicGateWayNHGObjectKey &key) const {
            if (key.type < type) {
                return true;
            }
            if (key.nexthop < nexthop) {
                return true;
            }
            if (key.vpnSid < vpnSid) {
                return true;
            }
            vector<std::pair<uint32_t, uint32_t>> keyGroupMember = key.groupMember;
            sort(keyGroupMember.begin(), keyGroupMember.end());
            string memberKey = "";
            string weightKey = "";

            for (auto it = keyGroupMember.begin(); it != keyGroupMember.end(); ++it) {
                if (!memberKey.empty()) {
                    memberKey += NHG_DELIMITER;
                    weightKey += NHG_DELIMITER;
                }
                memberKey += std::to_string(it->first);
                weightKey += std::to_string(it->second);
            }

            vector<std::pair<uint32_t, uint32_t>> myGroupMember = groupMember;
            sort(myGroupMember.begin(), myGroupMember.end());
            string memberStr = "";
            string weightStr = "";

            for (auto it = myGroupMember.begin(); it != myGroupMember.end(); ++it) {
                if (!memberStr.empty()) {
                    memberStr += NHG_DELIMITER;
                    weightStr += NHG_DELIMITER;
                }
                memberStr += std::to_string(it->first);
                weightStr += std::to_string(it->second);
            }

            if (memberKey < memberStr) {
                return true;
            }
            if (weightKey < weightStr) {
                return true;
            }
            return false;
        }

        static SonicGateWayNHGObjectKey createSonicGateWayNHGObjectKey(SonicGateWayNHGObject obj);

        static int createSonicGateWayNHGObjectKey(RIBNHGEntry *entry, SonicGateWayNHGObjectKey &key_out);
    };

    /* Sonic Gateway nexthop object */
    struct SonicGateWayNHGObject {
        sonicNhgObjType type;
        vector<std::pair<uint32_t, uint32_t>> groupMember;
        string nexthop;
        string vpnSid;
        string ifName;
        string segSrc;
        uint32_t ifIndex;
        uint32_t id;
    };

    /* Sonic ID allocator, used to allocate id for single type Sonic NHG Object */
    class SonicIDAllocator {
    public:
        SonicIDAllocator(string tableName) {
            g_id = 1;
            m_table_name = tableName;
        };

        uint32_t allocateID();

        void freeID(uint32_t id);

        int recoverSonicIDMapFromDB();

        bool isInUsed(uint32_t id);

    private:
        map<uint32_t, uint32_t> m_id_map;
        uint32_t g_id;
        string m_table_name;
    };

    /* Sonic ID Manager, used to manage all kinds of Sonic NHG id for Sonic NHG Object */
    class SonicIDMgr {
    public:
        SonicIDMgr(){};
        int init(vector<sonicNhgObjType> supportObj);
        u_int32_t allocateID(sonicNhgObjType type);
        void freeID(sonicNhgObjType type, uint32_t id);
        bool isSonicObjIDUsed(sonicNhgObjType type, uint32_t id);

    private:
        SonicIDAllocator *m_nhg_id_allocator = nullptr;
        SonicIDAllocator *m_pic_id_allocator = nullptr;
        SonicIDAllocator *getAllocator(sonicNhgObjType type);
    };

    /* Used to map Zebra NHG and Sonic NHG, not implemented */
    class NexthopMapKey {

    public:
        NexthopMapKey();

        NexthopMapKey(const NextHopGroupFull *nhg);

        string getNHGKey() { return m_address_key; };

        string getRIBKey() { return m_address_key; };

        string getSonicKey() { return m_address_key; };

    private:
        string m_address_key;
    };

    /* Sonic Gateway nexthop entry */
    class SonicGateWayNHGEntry {
    public:
        SonicGateWayNHGEntry(SonicNHGTable *mTable);
        ~SonicGateWayNHGEntry();
        static SonicGateWayNHGEntry *createSonicGateWayNHGEntry(SonicNHGTable *mTable);
        vector<FieldValueTuple> getFvVector();
        SonicGateWayNHGObject getNHG();
        int syncFvVector();
        int setEntry(SonicGateWayNHGObject nhg);
        void setRIBID(uint32_t id) {
            m_rib_id = id;
        };
        SonicGateWayNHGObjectKey getSonicGateWayObjKey() {
            return m_sonic_obj_key;
        };
        sonicNhgObjType getSonicGateWayObjType() {
            return m_sonic_obj.type;
        };
        sonicNhgObjType getType() {
            return m_sonic_obj.type;
        };
        uint32_t getSonicGateWayObjID() {
            return m_sonic_obj_id;
        };

    private:
        uint32_t m_rib_id;
        SonicGateWayNHGObjectKey m_sonic_obj_key;
        NexthopMapKey m_key;
        uint32_t m_sonic_obj_id;
        SonicGateWayNHGObject m_sonic_obj;
        set<uint32_t> m_rib_dependents;
        set<uint32_t> m_sonic_obj_dependents;
        set<uint32_t> m_rib_depends;
        set<pair<uint32_t, uint32_t>> m_group;
        SonicNHGTable *m_table;
        vector<FieldValueTuple> m_fvVector;

        int setSRv6GatewayEntry(SonicGateWayNHGObject nhg);
    };

    /* Sonic Gateway NHG table */
    class SonicNHGTable {

    public:
        SonicNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);
        ~SonicNHGTable(){

        };

        int addEntry(SonicGateWayNHGObject sonicObj);

        void delEntry(SonicGateWayNHGObject sonicObj);

        void delEntry(sonicNhgObjType type, uint32_t id);

        SonicGateWayNHGEntry *getEntry(SonicGateWayNHGObjectKey key);

        SonicGateWayNHGEntry *getEntry(sonicNhgObjType type, uint32_t id);

        int updateEntry(SonicGateWayNHGObject nhg);

        int writeToDB(SonicGateWayNHGEntry *entry);

        void removeFromDB(SonicGateWayNHGEntry *entry);

        void cleanUp();

    private:
        map<uint32_t, SonicGateWayNHGEntry *> m_pic_map;
        std::map<SonicGateWayNHGObjectKey, SonicGateWayNHGEntry *> m_sonic_nhg_map;
        ProducerStateTable m_pic_contextTable;
    };

    /* RIB NHG entry */
    class RIBNHGEntry {
    public:
        RIBNHGEntry(RIBNHGTable *table);

        ~RIBNHGEntry();

        /* static creation func */
        static RIBNHGEntry *createNHGEntry(RIBNHGTable *mTable);

        int createSonicNHGObjFromRIBEntry(SonicGateWayNHGObject &sonicNhgOut);

        int createSRv6GatewayObjFromRIBEntry(SonicGateWayNHGObject &sonicNhgOut);

        /* getter */
        set<uint32_t> getDependsID();

        set<uint32_t> getDependentsID();

        unordered_map<uint32_t, uint8_t> getGroup();

        unordered_map<uint32_t, uint8_t> getResolvedGroup();

        vector<FieldValueTuple> getFvVector();

        NexthopMapKey getKey();

        NextHopGroupFull getNHG();

        uint32_t getSonicObjID();

        uint32_t getRIBID();

        string getNextHopStr();

        string getVPNSIDStr();

        string getInterfaceNameStr();

        uint32_t getSonicGatewayObjID();

        sonicNhgObjType getSonicObjType();

        bool hasSonicObj();

        bool isEntryNeedOffload() {
            return !m_is_single;
        }

        bool isSingleNexthop() {
            return m_is_single;
        }

        /* setter */
        void
        setSonicObjId(uint32_t id);

        void setSonicGatewayObjId(uint32_t id);

        int setEntry(NextHopGroupFull nhg);

        void addDependentsMember(uint32_t id);

        void removeDependentsMember(uint32_t id);

        int updateEntryFromNHGFull(NextHopGroupFull newNHG, bool &updated, bool &updatedDependency);

    private:
        sonicNhgObjType m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
        bool m_has_sonic_obj;
        uint32_t m_rib_id = -1;
        RIBNHGTable *m_table = nullptr;
        string m_nexthop = "";
        string m_vpnSid = "";
        string m_segSrc = "";
        string m_ifName = "";
        vector<FieldValueTuple> m_fvVector;
        NexthopMapKey m_key;
        NextHopGroupFull m_nhg;
        unordered_map<uint32_t, uint8_t> m_group;
        unordered_map<uint32_t, uint8_t> m_resolvedGroup;
        set<uint32_t> m_depends;
        set<uint32_t> m_dependents;
        uint32_t m_sonic_obj_id = 0;
        uint32_t m_sonic_gateway_nhg_id = 0;
        bool m_has_Sonic_obj = false;
        bool m_is_single = true;

        int getNextHopGroupFields(NextHopGroupFull nhg, string &nexthops, string &weights);

        int getNextHopFields(NextHopGroupFull nhg, string &nexthops, string &ifnames, uint8_t af);

        int getNHGFields(NextHopGroupFull nhg, string &nexthop, string &ifnames, string &weights, uint8_t &af);

        bool compareDependsAndDependents(const NextHopGroupFull *newNHG, const NextHopGroupFull *oldNHG);

        int syncFvVector();

        unordered_map<uint32_t, uint8_t> getResolvedGroupFromNHGFull(NextHopGroupFull nhg);

        // void sortDependencyList(NextHopGroupFull nhg);
        void setNHGFull(NextHopGroupFull nhg);

        bool needCreateSonicGatewayNHGObj();
    };

    /* RIB NHG table */
    class RIBNHGTable {

    public:
        RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);

        ~RIBNHGTable(){

        };

        int addEntry(NextHopGroupFull nhg);

        int delEntry(uint32_t id);

        int updateEntry(NextHopGroupFull nhg, bool &updated);

        RIBNHGEntry *getEntry(uint32_t id);

        RIBNHGEntry *getEntry(std::string key);

        int addNHGDependents(set<uint32_t> depends, uint32_t id);

        void removeNHGDependents(set<uint32_t> depends, uint32_t id);

        bool isNHGExist(string key);

        bool isNHGExist(uint32_t id);

        int writeToDB(RIBNHGEntry *entry);

        void removeFromDB(RIBNHGEntry *entry);

        void cleanUp();

        void dump_table(string &ret);

        void
        diffDependency(set<uint32_t> oldSet, set<uint32_t> newSet, set<uint32_t> &addSet, set<uint32_t> &removeSet);

        // Not implemented
        vector<RIBNHGEntry *> getDepends(std::string key);

        vector<RIBNHGEntry *> getDependents(uint32_t id);

    private:
        map<uint32_t, RIBNHGEntry *> m_nhg_map;
        ProducerStateTable m_nexthop_groupTable;
    };

    class NHGMgr {
    public:
        NHGMgr(RedisPipeline *pipeline, const std::string &nexthopTableName, const std::string &picTableName, bool isStateTable);

        ~NHGMgr() {
            if (m_rib_nhg_table != nullptr) {
                m_rib_nhg_table->cleanUp();
                delete m_rib_nhg_table;
            }
            if (m_sonic_nhg_table != nullptr) {
                m_sonic_nhg_table->cleanUp();
                delete m_sonic_nhg_table;
            }
        };

        int addNHGFull(NextHopGroupFull nhg);

        int delNHGFull(uint32_t id);

        // Not implemented
        RIBNHGEntry *getRIBNHGEntryByKey(string key);

        RIBNHGEntry *getRIBNHGEntryByRIBID(uint32_t id);

        bool isSonicNHGIDInUsed(uint32_t id);

        bool isSonicGatewayNHGIDInUsed(sonicNhgObjType, uint32_t id);

        int createSonicNhgObject(RIBNHGEntry *entry);

        int updateSonicNhgObject(RIBNHGEntry *entry, uint32_t previousSonicObjID);

        //bool getIfName(int if_index, char *if_name, size_t name_len);
        void dumpZebraNhgTable(string &ret);

        // Not implemented
        void dump_sonic_nhg_table(string &ret);

        SonicGateWayNHGEntry *getSonicNHGByID(sonicNhgObjType type, uint32_t id);

        SonicGateWayNHGEntry *getSonicNHGByRIBID(uint32_t id);

        SonicGateWayNHGEntry *getSonicNHGByKey(SonicGateWayNHGObjectKey key);

    private:
        DBConnector *m_db;
        // Map zebra NHG id to received zebra_dplane_ctx + SONIC Context (a.k.a SONIC ZEBRA NHG)
        RIBNHGTable *m_rib_nhg_table;
        // Map SONiC NHG id to SONiC created NHG
        SonicNHGTable *m_sonic_nhg_table;

        // Mock Sonic ID Allocator
        SonicIDMgr m_sonic_id_manager;

        // Map nexthops in zebra_dplane_ctx to zebra NHG ID
        //map<NexthopMapKey, uint32_t> nexthop_key_2_rib_id_map;
        // NEXTHOP Address to SONiC NHG ID list mapping
        //map<NexthofpKey, uint32_t> nexthop_key_2_sonic_id_map;
    };

}// namespace swss
/*
template<>
class hash<swss::SonicGateWayNHGObjectKey> {
public:
    size_t operator()(const SonicGateWayNHGObjectKey &k) const {
        vector<pair<uint32_t, uint32_t>> groupMember = k.groupMember;
        sort(groupMember.begin(), groupMember.end());
        string member = "";
        string weight = "";

        for (auto member: groupMember) {
            if (!member.empty()) {
                member += NHG_DELIMITER;
                weight += NHG_DELIMITER;
            }
            member += std::to_string(member.first);
            weight += std::to_string(member.second);
        }

        return hash<string>()(k.nexthop) + hash<string>()(k.vpnSid) + hash<int>()(k.type) + hash<string>()(member) + hash<string>()(weight);
    }
};*/
#endif// NHGMGR_H
