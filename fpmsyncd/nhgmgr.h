
#ifndef NHGMGR_H
#define NHGMGR_H

#include "dbconnector.h"
#include "ipprefix.h"
#include "producerstatetable.h"
#include <nexthopgroup/nexthopgroupfull.h>
#include <nexthopgroup/nexthopgroupfull_json.h>

#include <string.h>

using namespace std;
namespace swss {
    using NextHopGroupFull = fib::NextHopGroupFull;
    using nh_grp_full = fib::nh_grp_full;

    enum NEXTHOP_KEY_TYPE {
        NEXTHOP_KEY_TYPE_ZEBRA,
        NEXTHOP_KEY_TYPE_SONIC,
    };

    class mockSonicIdMgr {
    public:
        mockSonicIdMgr() {
            g_id = 1;
        };
        uint32_t allocateID() {
            while (m_id_map.find(g_id) != m_id_map.end()) {
                if (g_id == 0) {
                    g_id = 1;
                } else {
                    g_id++;
                }
            }
            m_id_map[g_id] = 1;
            return g_id;
        };
        void freeID(uint32_t id) {
            if (m_id_map.find(id) != m_id_map.end()) {
                m_id_map.erase(id);
            }
        }

        bool isInUsed(uint32_t id) {
            if (m_id_map.find(id) != m_id_map.end()) {
                return true;
            }
            return false;
        }

    private:
        map<uint32_t, uint32_t> m_id_map;
        uint32_t g_id;
    };

    class NexthopKey {

    public:
        NexthopKey();
        NexthopKey(const NextHopGroupFull *nhg);
        string getNHGKey() { return m_address_key; };
        string getRIBKey(enum NEXTHOP_KEY_TYPE type);
        string getSonicKey() { return m_address_key; };
        string getNHGKey(enum NEXTHOP_KEY_TYPE m_address_key);

    private:
        string m_address_key;
    };


    // for pic. Not implemented
    class SonicNHGEntry {

    public:
        SonicNHGEntry();
        void createNHGEntry();

    private:
        NexthopKey m_key;
    };

    // TODO
    class SonicNHGObject {};

    class SonicNHGTable {

    public:
        SonicNHGTable();
        ~SonicNHGTable();
        int addEntry();
        int delEntry();
        SonicNHGEntry *getEntry(std::string key);
        SonicNHGEntry *getEntry(uint32_t id);

    private:
        map<uint32_t, SonicNHGEntry *> m_nhg_map;
        ProducerStateTable m_nexthop_groupTable;
    };

    class RIBNHGTable;

    class RIBNHGEntry {
    public:
        RIBNHGEntry(RIBNHGTable *table, NextHopGroupFull nhg);
        ~RIBNHGEntry();

        /* static creation func */
        static RIBNHGEntry *createNHGEntry(NextHopGroupFull nhg, RIBNHGTable *mTable);

        /* getter */
        set<uint32_t> getDependsID();
        set<uint32_t> getDependentsID();
        unordered_map<uint32_t, uint8_t> getGroup();
        unordered_map<uint32_t, uint8_t> getResolvedGroup();
        vector<FieldValueTuple> getFvVector();
        NexthopKey getKey();
        NextHopGroupFull getNHG();
        uint32_t getSoincObjID();
        uint32_t getRIBID();
        string getNextHopStr();

        /* setter */
        void
        setSonicObjId(uint32_t id);
        int setEntry(NextHopGroupFull nhg);
        void addDependentsMember(uint32_t id);
        void removeDependentsMember(uint32_t id);
        int updateEntryFromNHGFull(NextHopGroupFull newNHG, bool &updated, bool &updatedDependency);

    private:
        uint32_t m_rib_id = -1;
        RIBNHGTable *m_table;
        string m_nexthop;
        vector<FieldValueTuple> m_fvVector;
        NexthopKey m_key;
        NextHopGroupFull m_nhg;
        unordered_map<uint32_t, uint8_t> m_group;
        unordered_map<uint32_t, uint8_t> m_resolved_group;
        set<uint32_t> m_depends;
        set<uint32_t> m_dependents;
        uint32_t m_sonic_obj_id = -1;
        uint32_t m_sonic_gateway_nhg_id = -1;
        int getNextHopGroupFields(NextHopGroupFull nhg, string &nexthops, string &weights);
        int getNextHopFields(NextHopGroupFull nhg, string &nexthops, string &ifnames, uint8_t af);
        int getNHGFields(NextHopGroupFull nhg, string &nexthop, string &ifnames, string &weights, uint8_t &af);
        bool compareDependsAndDependents(const NextHopGroupFull *newNHG, const NextHopGroupFull *oldNHG);
        int syncFvVector();
        unordered_map<uint32_t, uint8_t> getResolvedGroupFromNHGFull(NextHopGroupFull nhg);
        // void sortDependencyList(NextHopGroupFull nhg);
        void setNHGFull(NextHopGroupFull nhg);
    };

    class RIBNHGTable {

    public:
        RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);
        ~RIBNHGTable(){

        };
        int addEntry(NextHopGroupFull nhg);
        int delEntry(uint32_t id);
        int updateEntry(NextHopGroupFull nhg);
        RIBNHGEntry *getEntry(uint32_t id);
        RIBNHGEntry *getEntry(std::string key);
        int addNHGDependents(set<uint32_t> depends, uint32_t id);
        void removeNHGDependents(set<uint32_t> depends, uint32_t id);
        bool isNHGExist(string key);
        bool isNHGExist(uint32_t id);
        int writeToDB(RIBNHGEntry *entry);
        void removeFromDB(RIBNHGEntry *entry);
        void dump_table(string &ret);
        void diffDependency(set<uint32_t> oldSet, set<uint32_t> newSet, set<uint32_t> &addSet, set<uint32_t> &removeSet);

        // Not implemented
        vector<RIBNHGEntry *> getDepends(std::string key);
        vector<RIBNHGEntry *> getDependents(uint32_t id);

    private:
        map<uint32_t, RIBNHGEntry *> m_nhg_map;
        ProducerStateTable m_nexthop_groupTable;
    };


    class NHGMgr {
    public:
        NHGMgr(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);
        ~NHGMgr() {
            if (m_rib_nhg_table != nullptr) {
                delete m_rib_nhg_table;
            }
        };
        int addNHGFull(NextHopGroupFull nhg);
        int delNHGFull(uint32_t id);
        RIBNHGEntry *getRIBNHGEntryByKey(string key);
        RIBNHGEntry *getRIBNHGEntryByRIBID(uint32_t id);
        //bool getIfName(int if_index, char *if_name, size_t name_len);
        void dump_zebra_nhg_table(string &ret);

        // Not implemented
        void dump_sonic_nhg_table(string &ret);
        SonicNHGObject *getSonicNHGByID(uint32_t id);
        SonicNHGObject *getSonicNHGByKey(std::string key);

    private:
        DBConnector *m_db;
        // Map zebra NHG id to received zebra_dplane_ctx + SONIC Context (a.k.a SONIC ZEBRA NHG)
        RIBNHGTable *m_rib_nhg_table;
        // Map SONiC NHG id to SONiC created NHG
        SonicNHGTable *m_sonic_nhg_table;
        // Map nexthops in zebra_dplane_ctx to zebra NHG ID
        //map<NexthopKey, uint32_t> nexthop_key_2_rib_id_map;
        // NEXTHOP Address to SONiC NHG ID list mapping
        //map<NexthofpKey, uint32_t> nexthop_key_2_sonic_id_map;

        // Mock Sonic ID Allocator
        mockSonicIdMgr m_sonic_id_manager;
    };

}// namespace swss

#endif// NHGMGR_H
