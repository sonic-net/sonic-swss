#ifndef NHGMGR_H
#define NHGMGR_H

#include "dbconnector.h"
#include "ipprefix.h"
#include "producerstatetable.h"
#include <nexthopgroup/nexthopgroupfull.h>
#include <nexthopgroup/nexthopgroupfull_json.h>
#include <nexthopgroup/nexthopgroup_debug.h>

#include <string.h>
#include <algorithm>
#include <set>

#define NHG_DELIMITER ','
#define NEXTHOP_GROUP_RECEIVED_FLAG (1 << 10)
#define CHECK_FLAG(V,F)      ((V) & (F))


/* Forward declaration for unit test friend access */
namespace ut_fpmsyncd { struct FpmSyncdNhgMgr; }

namespace swss {

using namespace std;

    using NextHopGroupFull = fib::NextHopGroupFull;
    using nh_grp_full = fib::nh_grp_full;

    /* Forward Declarations */
    class RIBNHGTable;
    class SonicPICContentTable;
    class RIBNHGEntry;
    struct SonicPICContentObject;

    /* description of sonic nexthop object creation info */
    struct SonicNHGObjectInfo {
        uint32_t id = 0;
        uint32_t refCount = 0;
        SonicNHGObjectInfo()  {
        }
        SonicNHGObjectInfo(uint32_t id, uint32_t refCount) : id(id), refCount(refCount) {
        }
    };

    /* Type of sonic nexthop object */
    enum sonicNhgObjType {

        /* NEXTHOP_GROUP_TABLE type */
        SONIC_NHG_OBJ_TYPE_NHG_NORMAL = 0,

        /* PIC_CONTEXT_TABLE type */
        SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC = 1,

        SONIC_NHG_OBJ_TYPE_MAX = 255,
    };

    /* Key Object used to hash Sonic PIC nexthop object */
    struct SonicNHGObjectKey {
        vector<std::pair<uint32_t, uint32_t>> groupMember;
        string nexthop;
        string vpnSid;
        string segSrc;
        string ifName;
        sonicNhgObjType type;

        std::vector<std::pair<uint32_t, uint32_t>> getSortedGroupMember() const {
            std::vector<std::pair<uint32_t, uint32_t>> sorted = groupMember;
            sort(sorted.begin(), sorted.end());
            return sorted;
        }

        bool operator<(const SonicNHGObjectKey &other) const {
            if (type != other.type) return type < other.type;
            if (nexthop != other.nexthop) return nexthop < other.nexthop;
            if (type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC && vpnSid != other.vpnSid)
                return vpnSid < other.vpnSid;
            if (segSrc != other.segSrc) return segSrc < other.segSrc;
            if (ifName != other.ifName) return ifName < other.ifName;

            return getSortedGroupMember() < other.getSortedGroupMember();
        }

        /*
         * compare operator of two SonicNHGObjectKey
         */
        bool operator==(const SonicNHGObjectKey &b) const {
            if (type != b.type) {
                return false;
            }
            if (nexthop != b.nexthop) {
                return false;
            }
            if (type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC && vpnSid != b.vpnSid) {
                return false;
            }
            if (segSrc != b.segSrc) {
                return false;
            }
            if (ifName != b.ifName) {
                return false;
            }
            if (groupMember.size() != b.groupMember.size()) {
                return false;
            }
            return getSortedGroupMember() == b.getSortedGroupMember();
        }

        bool operator!=(const SonicNHGObjectKey &b) const {
            return !(*this == b);
        }

        static SonicNHGObjectKey createSonicPICContentObjectKey(SonicPICContentObject obj);

        static int createSonicPICContentObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out);

        static void createSonicNormalNHGObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out);
    };

    /* Sonic PIC object */
    struct SonicPICContentObject {
        /*
         * type of Sonic PIC object
         * currently support three types:
         * SONIC_NHG_OBJ_TYPE_NHG_NORMAL: normal NHG Objects
         * SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: SRv6 VPN PIC Contexts
         */
        sonicNhgObjType type;

        /*
         * contain the <ribID, weight> pairs of Sonic PIC object
         */
        vector<std::pair<uint32_t, uint32_t>> groupMember;

        /*
         * nexthop of Sonic PIC object
         */
        string nexthop;

        /*
         * vpnSid of Sonic PIC object
         */
        string vpnSid;

        /*
         * ifName of Sonic PIC object
         */
        string ifName;

        /*
         * segSrc of Sonic PIC object
         * optional
         */
        string segSrc;

        /*
         * ifIndex of Sonic PIC object
         */
        uint32_t ifIndex;

        /*
         * ID of Sonic PIC object
         */
        uint32_t id;
    };

    /* Sonic ID allocator, used to allocate id for single type Sonic NHG Object */
    class SonicIDAllocator {
    public:
        SonicIDAllocator(string tableName) {
            g_id = 1;
            m_table_name = tableName;
        };

        /*
         * allocate Sonic Object ID
         * zero is reserved for invalid id
         */
        uint32_t allocateID();

        /*
         * free Sonic Object ID
         */
        void freeID(uint32_t id);

        /*
         * recover Sonic Object ID map from DB
         * used in warm reboot
         * not implemented
         */
        int recoverSonicIDMapFromDB();

        /*
         * check if the id is in used
         */
        bool isInUsed(uint32_t id);

    private:
        map<uint32_t, uint32_t> m_id_map;
        uint32_t g_id;
        string m_table_name;
    };

    /* Sonic ID Manager, used to manage all kinds of Sonic NHG id for Sonic NHG Object */
    class SonicIDMgr {
        /* Allow unit test fixture to access private members for white-box testing */
        friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

    public:

        SonicIDMgr(){};
        ~SonicIDMgr(){
            delete m_nhg_id_allocator;
            delete m_pic_id_allocator;
        };

        /*
         * initialize Sonic ID Manager
         * supportObj: supported Sonic Object type list
         * for now only support SONIC_NHG_OBJ_TYPE_NHG_NORMAL for normal NHG Objects
         * and SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC for SRv6 VPN PIC Contexts
         */
        int init(vector<sonicNhgObjType> supportObj);

        /*
         * allocate Sonic Object ID for specific type
         * zero is reserved for invalid id
         */
        u_int32_t allocateID(sonicNhgObjType type);

        /*
         * free Sonic Object ID for specific type
         */
        void freeID(sonicNhgObjType type, uint32_t id);

        /*
         * check if the id is in used for specific type
         */
        bool isSonicObjIDUsed(sonicNhgObjType type, uint32_t id);

    private:

        /*
         * Sonic ID Allocator for SONIC_NHG_OBJ_TYPE_NHG_NORMAL Objects
         */
        SonicIDAllocator *m_nhg_id_allocator = nullptr;

        /*
         * Sonic ID Allocator for SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC Objects
         */
        SonicIDAllocator *m_pic_id_allocator = nullptr;

        /*
         * get Sonic ID Allocator for specific type
         */
        SonicIDAllocator *getAllocator(sonicNhgObjType type);
    };

    /* Sonic PIC entry */
    class SonicPICContentEntry {
        /* Allow unit test fixture to access private members for white-box testing */
        friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

    public:
        SonicPICContentEntry(SonicPICContentTable *mTable);
        ~SonicPICContentEntry();

        /*
         * static creation func for SonicPICContentEntry
         */
        static SonicPICContentEntry *createSonicPICContentEntry(SonicPICContentTable *mTable);

        /*
         * get DB Fv Vector of SonicPICContentEntry
         */
        vector<FieldValueTuple> getFvVector();

        /*
         * get SonicPICContentObject of SonicPICContentEntry
         */
        SonicPICContentObject getNHG();

        /*
         * get the SonicNHGObjectKey of SonicPICContentEntry
         * SonicNHGObjectKey is used to map the SonicPICContentObject from Object fields
         */
        SonicNHGObjectKey getSonicPICContentObjKey() {
            return m_sonic_obj_key;
        };

        /*
         * get the type of SonicPICContentEntry
         * below are the supported types:
         *    SONIC_NHG_OBJ_TYPE_NHG_NORMAL
         *    SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC
         */
        sonicNhgObjType getType() {
            return m_sonic_obj.type;
        };

        /*
         * get the SonicPICContentObjectID of SonicPICContentEntry
         */
        uint32_t getSonicPicContentObjId() {
            return m_sonic_obj_id;
        };

        /*
         * set the value of entry from SonicPICContentObject
         * and sync the FV vector of entry
         */
        int setEntry(SonicPICContentObject nhg);

        /*
         * get the ref count of the entry
         */
        uint32_t getRefCount() {
            return m_ref_count;
        };
    private:
        /*
         * Sonic PIC Object Key of the entry
         */
        SonicNHGObjectKey m_sonic_obj_key;

        /*
         * Sonic PIC Object ID of the entry
         */
        uint32_t m_sonic_obj_id;

        /*
         * Sonic PIC Object of the entry
         */
        SonicPICContentObject m_sonic_obj;

        /*
         * Group of the entry
         * contain <ribID, weight> pair
         */
        set<pair<uint32_t, uint32_t>> m_group;

        /*
         * SonicNHGTable pointer of the entry
         */
        SonicPICContentTable *m_table;

        /*
         * DB FV vector of the entry
         */
        vector<FieldValueTuple> m_fvVector;

        /*
         * ref count of the entry
         */
        uint32_t m_ref_count = 1;

        /*
         * set the value of entry from SonicPICContentObject for SRv6 PIC Object
         */
        int setSRv6PICEntry(SonicPICContentObject nhg);

        /*
         * sync the FV vector of entry
         */
        int syncFvVector();

        /*
         * sync the FV vector of entry for SRv6 PIC Object
         */
        int syncFvVectorForSRv6PIC();
    };

    /* Sonic PIC content table */
    class SonicPICContentTable {

    public:
        SonicPICContentTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);
        ~SonicPICContentTable(){

        };

        /*
         * add entry to SonicNHGTable
         */
        int addEntry(SonicPICContentObject sonicObj);

        /*
         * delete entry from SonicNHGTable by SonicPICContentObject
         */
        void delEntry(SonicPICContentObject sonicObj);

        /*
         * delete entry from SonicNHGTable by type and sonic object id
         */
        void delEntry(sonicNhgObjType type, uint32_t id);

        /*
         * get entry from SonicNHGTable by type and sonic object id
         */
        SonicPICContentEntry *getEntry(sonicNhgObjType type, uint32_t id);

        /*
         * update entry in SonicNHGTable
         */
        int updateEntry(SonicPICContentObject nhg);

        /*
         * write entry to DB
         */
        int writeToDB(SonicPICContentEntry *entry);

        /*
         * remove entry from DB
         */
        void removeFromDB(SonicPICContentEntry *entry);

        /*
         * clean up SonicNHGTable
         */
        void cleanUp();

    private:

        /*
         * PIC context map of the table
         * contain <id, entry> pair
         */
        map<uint32_t, SonicPICContentEntry *> m_pic_map;

        /*
         * PIC context table
         */
        ProducerStateTable m_pic_contextTable;
    };

    /* RIB NHG entry */
    class RIBNHGEntry {
        /* Allow unit test fixture to access private members for white-box testing */
        friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

    public:
        RIBNHGEntry(RIBNHGTable *table);
        ~RIBNHGEntry();

        /*
         * static creation func
         */
        static RIBNHGEntry *createNHGEntry(RIBNHGTable *mTable);

        /*
         * create SonicPICContentObject from RIBNHGEntry
         */
        int createSonicPICContentObjectFromRIBEntry(SonicPICContentObject &sonicNhgOut);

        /*
         * create SonicPICContentObject for SRv6 PIC from RIBNHGEntry
         */
        int createSRv6PICObjFromRIBEntry(SonicPICContentObject &sonicNhgOut);

        /*
         * get the list of depends RIB ID
         */
        set<uint32_t> getDependsID();

        /*
         * get the list of dependents RIB ID
         */
        set<uint32_t> getDependentsID();

        /*
         * get the full group of RIBNHGEntry
         * contain <ribID, weight> pair
         */
        unordered_map<uint32_t, uint16_t> getGroup();

        /*
         * get the resolved group of RIBNHGEntry
         * contain <ribID, weight> pair
         */
        unordered_map<uint32_t, uint16_t> getResolvedGroup();

        /*
         * get the DB FV vector of RIBNHGEntry
         */
        vector<FieldValueTuple> getFvVector();

        /*
         * get the NextHopGroupFull Object of RIBNHGEntry
         */
        NextHopGroupFull getNHG();

        /*
         * get the Sonic Object ID of RIBNHGEntry
         */
        uint32_t getSonicObjID();

        /*
         * get the RIB ID of RIBNHGEntry
         */
        uint32_t getRIBID();

        /*
         * get the address family of RIBNHGEntry
         */
        uint8_t getAddressFamily();

        /*
         * get the nexthop str in FV Vector of RIBNHGEntry
         */
        string getNextHopStr();

        /*
         * get the vpn sid str in FV Vector of RIBNHGEntry
         * only for SRv6 Gateway
         */
        string getVPNSIDStr();

        /*
         * get the seg src str in FV Vector of RIBNHGEntry
         * only for SRv6 Gateway
         */
        string getSegSrcStr();

        /*
         * get the interface name str in FV Vector of RIBNHGEntry
         */
        string getInterfaceNameStr();

        /*
         * get the Sonic Gateway Object ID of RIBNHGEntry
         * for now only used for SRv6 VPN case
         */
        uint32_t getSonicPICObjID();

        /*
         * get the Sonic Gateway Object type of RIBNHGEntry
         * if not exist, return SONIC_NHG_OBJ_TYPE_NHG_NORMAL
         */
        sonicNhgObjType getSonicObjType();

        /*
         * check if RIBNHGEntry has Sonic Gateway Object
         */
        bool hasSonicPICObj();

        /*
         * check if RIBNHGEntry need write to DB
         */
        bool needCreateSonicObject() {
            return m_create_sonic_nhg_obj;
        }

        /*
         * check if RIBNHGEntry is single nexthop
         */
        bool isSingleNexthop() {
            return m_is_single;
        }

        /*
         * check if RIBNHGEntry uses shared Sonic NHG
         */
        bool isSharedSonicNHG() {
            return m_is_shared_sonic_nhg;
        }

        /*
         * set the Sonic NHG Object ID of RIBNHGEntry
         */
        void setSonicNHGObjId(uint32_t id);

        /*
         * set the Sonic Gateway Object ID of RIBNHGEntry
         */
        void setSonicPICObjId(uint32_t id);

        /*
         * set the value of entry from NextHopGroupFull Object
         * and sync the FV vector of entry
         */
        int setEntry(NextHopGroupFull nhg, uint8_t af);

        /*
         * set the enable flag true of RIBNHGEntry
         */
        void enableNHG();

        /*
         * set the enable flag false of RIBNHGEntry
         */
        void disableNHG();

        /*
         * get the NHG enable status of RIBNHGEntry
         */
        bool getNhgEnableStatus();

        /*
         * get the m_is_srv6_nhg fields to check if NHG has SRv6 Infos
         */
        bool isSRv6Nhg();

        SonicNHGObjectKey getSonicNHGObjectKey(){
            return m_sonic_nhg_key;
        }
        /*
         * update entry from NextHopGroupFull Object
         * out: updated, true if the entry is updated
         * out: updatedDependency, true if the entry dependency is updated
         */
        void checkNeedUpdate(NextHopGroupFull newNHG, uint8_t newAF, bool &updated);

    private:

        /*
         * Sonic Object type of the entry
         * currently support three types:
         * SONIC_NHG_OBJ_TYPE_NHG_NORMAL: normal NHG Objects
         * SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: SRv6 VPN PIC Contexts
         * default: SONIC_NHG_OBJ_TYPE_NHG_NORMAL
         */
        sonicNhgObjType m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;

        /*
         * RIB ID of the entry
         */
        uint32_t m_rib_id = 0;

        /*
         * RIB NHG table pointer of the entry
         */
        RIBNHGTable *m_table = nullptr;

        /*
         * Nexthop str in FV vector of the entry
         */
        string m_nexthop = "";

        /*
         * Interface name str in FV vector of the entry
         */
        string m_ifName = "";

        /*
         * address family of the entry
         */
        uint8_t m_af = 0;

        /*
         * FV vector of the entry
         */
        vector<FieldValueTuple> m_fvVector;

        /*
         * NextHopGroupFull Object of the entry
         */
        NextHopGroupFull m_nhg;

        /*
         * full group of the entry
         * contain <ribID, weight> pair
         */
        unordered_map<uint32_t, uint16_t> m_group;

        /*
         * resolved group of the entry
         * contain <ribID, weight> pair
         */
        unordered_map<uint32_t, uint16_t> m_resolvedGroup;

        /*
         * depends of the entry
         * contain ribID list
         */
        set<uint32_t> m_depends;

        /*
         * dependents of the entry
         * contain ribID list
         */
        set<uint32_t> m_dependents;

        /*
         * Sonic NHG Object ID of the entry
         */
        uint32_t m_sonic_obj_id = 0;

        /*
         * single nexthop flag of the entry
         */
        bool m_is_single = true;

        /*
         * enable flag of the entry
         * default: true
         * updated in back walk process
         */
        bool m_enable = true;

        /*
         *  Shared Sonic NHG Object flag of the entry.
         */
        bool m_is_shared_sonic_nhg = false;

        /*Sonic Gateway Obj fields */
        /*
         * has Sonic Gateway Object flag of the entry
         */
        bool m_has_sonic_gateway_obj = false;

        /*
         * True if with srv6 information or member with srv6 information
         */
        bool m_is_srv6_nhg = false;

        /*
         * Sonic PIC Content Object ID of the entry
         */
        uint32_t m_sonic_gateway_nhg_id = 0;

        /*
         * VPN SID str of the entry
         * only for SRv6 Gateway
         */
        string m_vpnSid = "";

        /*
         * Segment Source str of the entry
         * only for SRv6 Gateway
         */
        string m_segSrc = "";

        string m_weight = "";

        bool m_create_sonic_nhg_obj = false;

        /*
         * Sonic NHG Object key of the entry
         */
        SonicNHGObjectKey m_sonic_nhg_key;

        /*
         * calculate FV vector fields of multi NHG entry
         */
        int getNextHopGroupFields();

        /*
         * calculate FV vector fields of single NHG entry
         */
        int getNextHopFields();

        /*
         * calculate FV vector fields for entry
         */
        int getNHGFields();

        /*
         * fill the FV vector of the entry
         */
        int syncFvVector();

        /*
         * get the resolved group from NextHopGroupFull Object
         */
        int getResolvedGroupFromNHGFull();

        /*
        * check if need create Sonic PIC Content Object and Sonic gateway object type
        */
        void checkNeedCreateSonicPICObj();

        /*
         * check if need create Sonic PIC Content Object
         */
        void checkNeedCreateSonicNHGObj();
    };

    /* RIB NHG table */
    class RIBNHGTable {
        /* Allow unit test fixture to access private members for white-box testing */
        friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

    public:
        RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);
        ~RIBNHGTable(){

        };

        // add entry into table
        int addEntry(NextHopGroupFull nhg, uint8_t af);

        // delete entry from table
        int delEntry(uint32_t id);

        // update entry in table
        int updateEntry(NextHopGroupFull nhg, uint8_t af, bool &updated);

        // get entry from table by rib ID
        RIBNHGEntry *getEntry(uint32_t id);

        // check if NHG entry exist in table by rib ID
        bool isNHGExist(uint32_t id);

        // write the NHG entry to DB
        int writeToDB(RIBNHGEntry *entry);

        // remove the NHG entry from DB
        void removeFromDB(uint32_t id);

        // clean up all the entry the table
        void cleanUp();

        // insert shared Sonic NHG Object into m_created_shared_nhg_map
        void insertCreatedSharedNHGObject(SonicNHGObjectKey key, uint32_t id);

        // insert created Sonic NHG Object into m_sonic_nhg_id_2_rib_nhg_id_map
        void insertCreatedNhgObject(uint32_t sonicNhgId, uint32_t ribNhgId);

        // get created Sonic NHG Object ID from m_created_shared_nhg_map, return 0 if not exist
        int getCreatedSharedNHGObjectID(SonicNHGObjectKey key);

        void addSonicNHGObjectRef(SonicNHGObjectKey key);

        void subSonicNHGObjectRef(SonicNHGObjectKey key);

        void setSonicIDManager(SonicIDMgr *sonic_id_manager){
            m_sonic_id_manager = sonic_id_manager;
        }

    private:
        SonicIDMgr *m_sonic_id_manager = nullptr;

        map<uint32_t, RIBNHGEntry *> m_nhg_map;

        /* store created Sonic NHG Object */
        map<SonicNHGObjectKey, SonicNHGObjectInfo> m_created_shared_nhg_map;

        /*
         * store Sonic NHG Object ID to RIB NHG ID mapping
         * not include shared NHG Object
         */
        map<uint32_t , uint32_t> m_sonic_nhg_id_2_rib_nhg_id_map;

        ProducerStateTable m_nexthop_groupTable;
    };

    class NHGMgr {
        /* Allow unit test fixture to access private members for white-box testing */
        friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

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

        // add NHG into FIB block
        int addNHGFull(NextHopGroupFull nhg, uint8_t af);

        // delete NHG from FIB block
        int delNHGFull(uint32_t id);

        // get RIBNHGEntry by RIB ID
        RIBNHGEntry *getRIBNHGEntryByRIBID(uint32_t id);

        // check if Sonic NHG ID is in used
        bool isSonicNHGIDInUsed(uint32_t id);

        // check if Sonic PIC Content ID is in used
        bool isSonicPICIDInUsed(sonicNhgObjType, uint32_t id);

        // get SonicPICContentEntry by Sonic Object type and id
        SonicPICContentEntry *getSonicPICByID(sonicNhgObjType type, uint32_t id);

        // get SonicPICContentEntry by RIB id
        SonicPICContentEntry *getSonicPICByRIBID(uint32_t id);

        // Not implemented
        void dump_sonic_nhg_table(string &ret);

        // Not implemented
        void dumpZebraNhgTable(string &ret);

        // Not implemented
        RIBNHGEntry *getRIBNHGEntryByKey(string key);

    private:
        // Map zebra NHG id to received zebra_dplane_ctx + SONIC Context (a.k.a SONIC ZEBRA NHG)
        RIBNHGTable *m_rib_nhg_table;

        // Map SONiC NHG id to SONiC Gateway Object
        SonicPICContentTable *m_sonic_nhg_table;

        // Manage Sonic Object ID allocation
        SonicIDMgr m_sonic_id_manager;

        // process of adding new NHG Full
        int addNewNHGFull(NextHopGroupFull nhg, uint8_t af);

        // process of updating existing NHG Full
        int updateExistingNHGFull(NextHopGroupFull nhg, uint8_t af);

        // create Sonic PIC Content Object
        int createSonicPICObject(RIBNHGEntry *entry);

        // update Sonic PIC Content Object
        int updateSonicPICObject(RIBNHGEntry *entry, uint32_t previousSonicObjID);

        // dump NHG Group Full for debugging
        void dumpNHGGroupFull(NextHopGroupFull nhg);

    };

}// namespace swss

#endif// NHGMGR_H
