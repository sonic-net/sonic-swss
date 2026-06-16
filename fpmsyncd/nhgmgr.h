#ifndef NHGMGR_H
#define NHGMGR_H

#include "dbconnector.h"
#include "ipprefix.h"
#include "producerstatetable.h"
#include <nexthopgroup/nexthopgroupfull.h>
#include <nexthopgroup/nexthopgroupfull_json.h>
#include <nexthopgroup/nexthopgroup_debug.h>

#include <string.h>
#include <string>
#include <algorithm>
#include <memory>
#include <set>

#define NHG_DELIMITER ','
/*
 * Aligns with FRR's NEXTHOP_GROUP_RECEIVED_FROM_EXTERNAL flag.
 * Indicates the NHG was received from an external protocol (e.g., BGP).
 */
#define NEXTHOP_GROUP_RECEIVED_FROM_EXTERNAL (1 << 10)
#define CHECK_FLAG(V,F)      ((V) & (F))


/* Forward declaration for unit test friend access */
namespace ut_fpmsyncd { struct FpmSyncdNhgMgr; }

namespace swss {

using namespace std;

using NextHopGroupFull = fib::NextHopGroupFull;
using nh_grp_full = fib::nh_grp_full;

/* Type aliases for clarity and type safety */
using ribID = uint32_t;
using sonicObjectID = uint32_t;
using nhgWeight = uint16_t;
using idWeightPair = pair<uint32_t, nhgWeight>;

/* Forward Declarations */
class RIBNHGTable;
class SonicPICContentTable;
class RIBNHGEntry;
struct SonicPICContentObject;

/*
 * Sonic NHG object creation info: tracks the allocated Sonic object ID
 * and the number of RIB entries sharing this object (reference count).
 */
struct SonicNHGObjectInfo {
    sonicObjectID id = 0;         /* Sonic NHG object ID in APP_DB */
    uint32_t refCount = 0;   /* Number of RIB entries referencing this object */
    SonicNHGObjectInfo()  {
    }
    SonicNHGObjectInfo(sonicObjectID id, uint32_t refCount) : id(id), refCount(refCount) {
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

/*
 * Key Object used to identify/deduplicate Sonic NHG/PIC objects.
 * Two entries with the same key share the same Sonic object in APP_DB.
 * groupMember is stored as std::set (sorted) to avoid repeated sorting
 * during comparison operations.
 */
struct SonicNHGObjectKey {
    set<idWeightPair> groupMember;
    string nexthop;
    string vpnSid;
    string segSrc;
    string ifName;
    sonicNhgObjType type;

    bool operator<(const SonicNHGObjectKey &other) const {
        if (type != other.type) return type < other.type;
        if (nexthop != other.nexthop) return nexthop < other.nexthop;
        if (type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC && vpnSid != other.vpnSid)
            return vpnSid < other.vpnSid;
        if (segSrc != other.segSrc) return segSrc < other.segSrc;
        if (ifName != other.ifName) return ifName < other.ifName;

        return groupMember < other.groupMember;
    }

    /*
     * compare operator of two SonicNHGObjectKey
     */
    bool operator==(const SonicNHGObjectKey &b) const {
        if (type != b.type) return false;
        if (nexthop != b.nexthop) return false;
        if (type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC && vpnSid != b.vpnSid) return false;
        if (segSrc != b.segSrc) return false;
        if (ifName != b.ifName) return false;
        return groupMember == b.groupMember;
    }

    bool operator!=(const SonicNHGObjectKey &b) const {
        return !(*this == b);
    }

    static SonicNHGObjectKey createSonicPICContentObjectKey(SonicPICContentObject obj);

    static int createSonicPICContentObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out);

    static void createSonicNormalNHGObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out);
};

/*
 * SonicPICContentObject is used to refer to the pic context object information
 * that we want to create at the nhg and sonic level of a SRV6VPN.
 * We use this object to convey the corresponding information of pic object we created.
 * Each sonic content response to the one SonicPICContentObject.
 */
struct SonicPICContentObject {
    /*
     * type of Sonic PIC object
     * SONIC_NHG_OBJ_TYPE_NHG_NORMAL: normal NHG objects (NEXTHOP_GROUP_TABLE)
     * SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC: SRv6 VPN PIC objects (PIC_CONTEXT_TABLE)
     */
    sonicNhgObjType type;

    /*
     * <ribID, weight> pairs of member Sonic PIC objects (for multi-hop groups).
     * Stored as std::set to maintain sorted order.
     */
    set<idWeightPair> groupMember;

    string nexthop;   /* Gateway IP address string */
    string vpnSid;    /* SRv6 VPN SID (only for SRv6 type) */
    string ifName;    /* Outgoing interface name */
    string segSrc;    /* SRv6 segment source (optional, only for SRv6 type) */
    uint32_t ifIndex = 0;  /* Interface index */
    sonicObjectID id = 0;       /* Allocated Sonic object ID in APP_DB */
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
    sonicObjectID allocateID();

    /*
     * free Sonic Object ID
     */
    void freeID(sonicObjectID id);

    /*
     * check if the id is in used
     */
    bool isInUsed(sonicObjectID id);

private:
    map<sonicObjectID, bool> m_id_map;
    sonicObjectID g_id;
    string m_table_name;
};


    /* Sonic ID Manager, used to manage all kinds of Sonic NHG id for Sonic NHG Object */
class SonicIDMgr {
    /* Allow unit test fixture to access private members for testing */
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
    sonicObjectID allocateID(sonicNhgObjType type);

    /*
     * free Sonic Object ID for specific type
     */
    void freeID(sonicNhgObjType type, sonicObjectID id);

    /*
     * check if the id is in used for specific type
     */
    bool isSonicObjIDUsed(sonicNhgObjType type, sonicObjectID id);

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
    /* Allow unit test fixture to access private members for testing */
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
    vector<FieldValueTuple> getFvVector() { return m_fvVector; }

    /*
     * get SonicPICContentObject of SonicPICContentEntry
     */
    SonicPICContentObject getObj() { return m_sonic_pic_obj; }

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
        return m_sonic_pic_obj.type;
    };

    /*
     * get the SonicPICContentObjectID of SonicPICContentEntry
     */
    sonicObjectID getSonicPicContentObjId() {
        return m_sonic_obj_id;
    };

    /*
     * set the value of entry from SonicPICContentObject
     * and sync the FV vector of entry
     */
    int setEntry(const SonicPICContentObject &nhg);

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
    sonicObjectID m_sonic_obj_id;

    /*
     * Sonic PIC Object of the entry
     */
    SonicPICContentObject m_sonic_pic_obj;

    /*
     * Group of the entry
     * contain <ribID, weight> pair
     */
    set<idWeightPair> m_group;

    /*
     * SonicPICTable pointer of the entry
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
    int setSRv6PICEntry(const SonicPICContentObject &nhg);

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
     * add entry to SonicPICTable
     */
    int addEntry(const SonicPICContentObject &sonicObj);

    /*
     * delete entry from SonicPICTable by type and sonic object id
     */
    void delEntry(sonicNhgObjType type, sonicObjectID id);

    /*
     * get entry from SonicPICTable by type and sonic object id
     */
    SonicPICContentEntry *getEntry(sonicNhgObjType type, sonicObjectID id);

    /*
     * write entry to DB
     */
    int writeToDB(SonicPICContentEntry *entry);

    /*
     * remove entry from DB
     */
    void removeFromDB(SonicPICContentEntry *entry);

    /*
     * clean up SonicPICTable
     */
    void cleanUp();

private:

    /*
     * PIC context map of the table
     * contain <id, entry> pair
     */
    map<sonicObjectID, SonicPICContentEntry *> m_pic_map;

    /*
     * PIC context table
     */
    ProducerStateTable m_pic_contextTable;
};

/* RIB NHG entry */
class RIBNHGEntry {
    /* Allow unit test fixture to access private members for testing */
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
    set<ribID> getDependsID() { return m_depends; }

    /*
     * get the list of dependents RIB ID
     */
    set<ribID> getDependentsID() { return m_dependents; }

    /*
     * get the full group of RIBNHGEntry
     * contain <ribID, weight> pair
     */
    set<idWeightPair> getGroup() { return m_group; }

    /*
     * get the resolved group of RIBNHGEntry
     * contain <ribID, weight> pair
     */
    set<idWeightPair> getResolvedGroup() { return m_resolvedGroup; }

    /*
     * get the DB FV vector of RIBNHGEntry
     */
    vector<FieldValueTuple> getFvVector() { return m_fvVector; }

    /*
     * get the NextHopGroupFull Object of RIBNHGEntry
     */
    NextHopGroupFull getNHG() { return m_nhg; }

    /*
     * get the Sonic Object ID of RIBNHGEntry
     */
    sonicObjectID getSonicObjID() { return m_sonic_obj_id; }

    /*
     * get the RIB ID of RIBNHGEntry
     */
    ribID getRIBID() { return m_rib_id; }

    /*
     * get the address family of RIBNHGEntry
     */
    uint8_t getAddressFamily() { return m_af; }

    /*
     * get the nexthop str in FV Vector of RIBNHGEntry
     */
    string getNextHopStr() { return m_nexthop; }

    /*
     * get the vpn sid str in FV Vector of RIBNHGEntry
     * only for SRv6 Gateway
     */
    string getVPNSIDStr() { return m_vpnSid; }

    /*
     * get the seg src str in FV Vector of RIBNHGEntry
     * only for SRv6 Gateway
     */
    string getSegSrcStr() { return m_segSrc; }

    /*
     * get the interface name str in FV Vector of RIBNHGEntry
     */
    string getInterfaceNameStr(){ return m_ifName; }

    /*
     * get the sonic pic Object ID of RIBNHGEntry
     * for now only used for SRv6 VPN case
     */
    sonicObjectID getSonicPICObjID(){ return m_sonic_pic_obj_id;}

    /*
     * get the sonic pic Object type of RIBNHGEntry
     * if not exist, return SONIC_NHG_OBJ_TYPE_NHG_NORMAL
     */
    sonicNhgObjType getSonicObjType(){
        return m_sonic_obj_type;
    }

    /*
     * check if RIBNHGEntry has sonic pic Object
     */
    bool hasSonicPICObj() { return m_has_sonic_pic_obj; }

    /*
     * check if RIBNHGEntry need write to DB
     */
    bool needCreateSonicObject() {  return m_create_sonic_nhg_obj; }

    /*
     * check if RIBNHGEntry is single nexthop
     */
    bool isSingleNexthop() { return m_is_single;}

    /*
     * check if RIBNHGEntry uses shared Sonic NHG
     */
    bool isSharedSonicNHG() { return m_is_shared_sonic_nhg; }

    /*
     * set the Sonic NHG Object ID of RIBNHGEntry
     */
    void setSonicNHGObjId(sonicObjectID id) { m_sonic_obj_id = id; }

    /*
     * set the sonic pic Object ID of RIBNHGEntry
     */
    void setSonicPICObjId(sonicObjectID id) { m_sonic_pic_obj_id = id; }

    /*
     * set the value of entry from NextHopGroupFull Object
     * and sync the FV vector of entry
     */
    int setEntry(const NextHopGroupFull &nhg, uint8_t af);

    /*
     * set the enable flag true of RIBNHGEntry
     */
    void enableNHG() { m_enable = true; }

    /*
     * set the enable flag false of RIBNHGEntry
     */
    void disableNHG() { m_enable = false; }

    /*
     * get the NHG enable status of RIBNHGEntry
     */
    bool getNhgEnableStatus() { return m_enable; }

    /*
     * get the m_is_srv6_nhg fields to check if NHG has SRv6 Infos
     */
    bool isSRv6Nhg() { return m_is_srv6_nhg; }

    SonicNHGObjectKey getSonicNHGObjectKey(){
        return m_sonic_nhg_key;
    }

    /*
     * update entry from NextHopGroupFull Object
     * out: updated, true if the entry is updated
     * out: updatedDependency, true if the entry dependency is updated
     */
    bool checkNeedUpdate(const NextHopGroupFull &newNHG, uint8_t newAF);

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
    ribID m_rib_id = 0;

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
    set<idWeightPair> m_group;

    /*
     * resolved group of the entry
     * contain <ribID, weight> pair
     */
    set<idWeightPair> m_resolvedGroup;

    /*
     * depends of the entry
     * contain ribID list
     */
    set<ribID> m_depends;

    /*
     * dependents of the entry
     * contain ribID list
     */
    set<ribID> m_dependents;

    /*
     * Sonic NHG Object ID of the entry
     */
    sonicObjectID m_sonic_obj_id = 0;

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

    /*sonic pic Obj fields */
    /*
     * has sonic pic Object flag of the entry
     */
    bool m_has_sonic_pic_obj = false;

    /*
     * True if with srv6 information or member with srv6 information
     */
    bool m_is_srv6_nhg = false;

    /*
     * Sonic PIC Content Object ID of the entry
     */
    sonicObjectID m_sonic_pic_obj_id = 0;

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
    * check if need create Sonic PIC Content Object and sonic pic object type
    */
    void checkNeedCreateSonicPICObj();

    /*
     * check if need create Sonic PIC Content Object
     */
    void checkNeedCreateSonicNHGObj();
};

/* RIB NHG table */
class RIBNHGTable {
    /* Allow unit test fixture to access private members for testing */
    friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

public:
    RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable);
    ~RIBNHGTable(){

    };

    // add entry into table
    int addEntry(const NextHopGroupFull &nhg, uint8_t af);

    // delete entry from table
    void delEntry(ribID id);

    // update entry in table
    int updateEntry(const NextHopGroupFull &nhg, uint8_t af, bool &updated);

    // get entry from table by rib ID
    RIBNHGEntry *getEntry(ribID id);

    // check if NHG entry exist in table by rib ID
    bool isNHGExist(ribID id);

    // write the NHG entry to DB
    int writeToDB(RIBNHGEntry *entry);

    // remove the NHG entry from DB
    void removeFromDB(sonicObjectID id);

    // clean up all the entry the table
    void cleanUp();

    // insert shared Sonic NHG Object into m_created_shared_nhg_map
    void insertCreatedSharedNHGObject(SonicNHGObjectKey key, sonicObjectID id);

    // insert created Sonic NHG Object into m_sonic_nhg_id_2_rib_nhg_id_map
    void insertCreatedNhgObject(sonicObjectID sonicNhgId, ribID ribNhgId);

    // get created Sonic NHG Object ID from m_created_shared_nhg_map, return 0 if not exist
    int getCreatedSharedNHGObjectID(SonicNHGObjectKey key);

    void addSonicNHGObjectRef(SonicNHGObjectKey key);

    void subSonicNHGObjectRef(SonicNHGObjectKey key);

    void setSonicIDManager(SonicIDMgr *sonic_id_manager){
        m_sonic_id_manager = sonic_id_manager;
    }

private:

    SonicIDMgr *m_sonic_id_manager = nullptr;

    map<ribID, RIBNHGEntry *> m_nhg_map;

    /* store created Sonic NHG Object */
    map<SonicNHGObjectKey, SonicNHGObjectInfo> m_created_shared_nhg_map;

    /*
     * store Sonic NHG Object ID to RIB NHG ID mapping
     * not include shared NHG Object
     */
    map<sonicObjectID, ribID> m_sonic_nhg_id_2_rib_nhg_id_map;

    ProducerStateTable m_nexthop_groupTable;
};

class NHGMgr {
    /* Allow unit test fixture to access private members for testing */
    friend struct ut_fpmsyncd::FpmSyncdNhgMgr;

public:
    NHGMgr(RedisPipeline *pipeline, const std::string &nexthopTableName, const std::string &picTableName, bool isStateTable);
    ~NHGMgr() {
        if (m_rib_nhg_table != nullptr) {
            m_rib_nhg_table->cleanUp();
            delete m_rib_nhg_table;
        }
        if (m_sonic_pic_table != nullptr) {
            m_sonic_pic_table->cleanUp();
            delete m_sonic_pic_table;
        }
    };

    // add NHG into FIB block
    int addNHGFull(const NextHopGroupFull &nhg, uint8_t af);

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

private:

    // Map zebra NHG id to received zebra_dplane_ctx + SONIC Context (a.k.a SONIC ZEBRA NHG)
    RIBNHGTable *m_rib_nhg_table;

    // Map SONiC NHG id to sonic pic Object
    SonicPICContentTable *m_sonic_pic_table;

    // Manage Sonic Object ID allocation
    SonicIDMgr m_sonic_id_manager;

    // process of adding new NHG Full
    int addNewNHGFull(const NextHopGroupFull &nhg, uint8_t af);

    // process of updating existing NHG Full
    int updateExistingNHGFull(const NextHopGroupFull &nhg, uint8_t af);

    // create Sonic PIC Content Object
    int createSonicPICObject(RIBNHGEntry *entry);

    // dump NHG Group Full for debugging
    void dumpNHGGroupFull(const NextHopGroupFull &nhg);

};

}// namespace swss

#endif// NHGMGR_H
