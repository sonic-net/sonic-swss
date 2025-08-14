
#ifndef NHGMGR_H
#define NHGMGR_H

#include "dbconnector.h"
#include "ipprefix.h"
#include "nexthopgroupfull.h"
#include "producerstatetable.h"

#include <string.h>

using namespace std;
namespace swss {

    enum NEXTHOP_KEY_TYPE {
        NEXTHOP_KEY_TYPE_ZEBRA,
        NEXTHOP_KEY_TYPE_SONIC,
    };
    class MockSonicIDMgr {
    public:
        MockSonicIDMgr() {
            g_id = 1;
        };
        int allocateID() {
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

    private:
        map<uint32_t, uint32_t> m_id_map;
        uint32_t g_id;
    };
    class NexthopKey {

    public:
        NexthopKey();
        NexthopKey(const NextHopGroupFull *nhg);
        string getNhgKey() { return m_address_key; };
        string getRIBKey(enum NEXTHOP_KEY_TYPE type);
        string getSonicKey() { return m_address_key; };
        string getNhgKey(enum NEXTHOP_KEY_TYPE m_address_key);

    private:
        string m_address_key;
    };


    // for pic. Not implemented
    class SonicNHGEntry {

    public:
        SonicNHGEntry();
        void create_nhg_entry();

    private:
        NexthopKey m_key;
    };

    // TODO
    class SonicNHGObject {};

    class SonicNHGTable {

    public:
        SonicNHGTable();
        ~SonicNHGTable();
        int addNhg();
        int delNhg();
        SonicNHGEntry *getEntry(std::string key);
        SonicNHGEntry *getEntry(uint32_t id);

    private:
        map<uint32_t, SonicNHGEntry *> m_nhg_map;
        ProducerStateTable m_nexthop_groupTable;
    };
    class RIBNhgTable;
    class RIBNhgEntry {
    public:
        RIBNhgEntry(RIBNhgTable *table, NextHopGroupFull nhg);
        ~RIBNhgEntry();
        vector<RIBNhgEntry *> getDependsID();
        vector<RIBNhgEntry *> getDependentsID();
        vector<pair<uint32_t, uint8_t>> getGroup();
        vector<pair<uint32_t, uint8_t>> getResolvedGroup();
        vector<FieldValueTuple> getFvVector();
        int updateEntryFromNHGFull(NextHopGroupFull new_nhg, bool &updated);
        bool compareDependsAndDependents(const NextHopGroupFull *new_nhg, const NextHopGroupFull *old_nhg);
        int syncFvVector();
        NexthopKey getKey();
        NextHopGroupFull getNhg();
        int setEntry(NextHopGroupFull nhg);
        static RIBNhgEntry *create_nhg_entry(NextHopGroupFull nhg, RIBNhgTable *m_table);
        int getNextHopGroupFields(string &nexthops, string &ifnames, string &weights, uint8_t af);
        string getNexthop();
        void setNHGFull(NextHopGroupFull nhg);

    private:
        RIBNhgTable *m_table;
        bool isInstall;
        vector<FieldValueTuple> m_fvVector;
        NexthopKey m_key;
        string m_nexthop;
        uint8_t m_af;
        NextHopGroupFull m_nhg;
        vector<pair<uint32_t, uint8_t>> m_group;
        vector<pair<uint32_t, uint8_t>> m_resolved_group;
        vector<RIBNhgEntry *> m_depends;
        vector<RIBNhgEntry *> m_dependents;
        uint32_t m_sonic_id = -1;
        uint32_t m_sonic_gateway_nhg_id = -1;
    };

    class RIBNhgTable {

    public:
        RIBNhgTable(RedisPipeline *pipeline, const std::string &tableName, bool is_state_table);
        ~RIBNhgTable(){

        };
        int addNhg(NextHopGroupFull nhg);
        int delNhg(uint32_t id);
        int updateNhg(NextHopGroupFull nhg);
        RIBNhgEntry *getEntry(uint32_t id);
        RIBNhgEntry *getEntry(std::string key);
        void add_nhg_dependents(RIBNhgEntry *entry);
        void remove_nhg_dependents(RIBNhgEntry *entry);
        bool isNhgExist(string key);
        bool isNhgExist(uint32_t id);
        int writeToDB(RIBNhgEntry *entry);
        void removeFromDB(RIBNhgEntry *entry);
        void dump_table(string &ret);

        // Not implemented
        vector<RIBNhgEntry *> getDepends(std::string key);
        vector<RIBNhgEntry *> getDependents(uint32_t id);

    private:
        map<uint32_t, RIBNhgEntry *> m_nhg_map;
        ProducerStateTable m_nexthop_groupTable;
    };


    class NHGMgr {
    public:
        NHGMgr(RedisPipeline *pipeline, const std::string &tableName, bool is_state_table);
        ~NHGMgr() {
            if (m_rib_nhg_table != nullptr) {
                delete m_rib_nhg_table;
            }
        };
        int addNHGFull(NextHopGroupFull nhg);
        int delNHGFull(uint32_t id);
        RIBNhgEntry *getRIBNhgEntryByKey(string key);
        RIBNhgEntry *getRIBNhgEntryByRIBID(uint32_t id);
        //bool getIfName(int if_index, char *if_name, size_t name_len);
        void dump_zebra_nhg_table(string &ret);

        // Not implemented
        void dump_sonic_nhg_table(string &ret);
        SonicNHGObject *getSonicNHGByID(uint32_t id);
        SonicNHGObject *getSonicNHGByKey(std::string key);

    private:
        DBConnector *m_db;
        // Map zebra NHG id to received zebra_dplane_ctx + SONIC Context (a.k.a SONIC ZEBRA NHG)
        RIBNhgTable *m_rib_nhg_table;
        // Map SONiC NHG id to SONiC created NHG
        SonicNHGTable *m_sonic_nhg_table;
        // Map nexthops in zebra_dplane_ctx to zebra NHG ID
        //map<NexthopKey, uint32_t> nexthop_key_2_rib_id_map;
        // NEXTHOP Address to SONiC NHG ID list mapping
        //map<NexthofpKey, uint32_t> nexthop_key_2_sonic_id_map;

        // Mock Sonic ID Allocator
        MockSonicIDMgr m_sonic_id_manager;
    };

}// namespace swss

#endif// NHGMGR_H
