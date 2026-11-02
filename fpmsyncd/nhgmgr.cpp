#include "nhgmgr.h"
#include "logger.h"
#include <string.h>


using namespace std;
using namespace swss;

namespace {
/*
 * Thread-safe SWSS logger callback function.
 * All sonic_fib log messages will be forwarded to SWSS logger.
 */
// ADD THIS ATTRIBUTE BEFORE THE FUNCTION DEFINITION:
__attribute__((format(printf, 5, 0)))
void swssLogBridge(fib::LogLevel level, const char* file, int line,
                   const char* func, const char* format, va_list args) {
    // Map fib levels → SWSS levels
    swss::Logger::Priority swssLevel;
    switch (level) {
        case fib::LogLevel::DEBUG: swssLevel = swss::Logger::SWSS_DEBUG; break;
        case fib::LogLevel::INFO:  swssLevel = swss::Logger::SWSS_NOTICE; break;
        case fib::LogLevel::WARN:  swssLevel = swss::Logger::SWSS_WARN; break;
        case fib::LogLevel::ERROR: swssLevel = swss::Logger::SWSS_ERROR; break;
        default: swssLevel = swss::Logger::SWSS_WARN;
    }

    // Format message into buffer, max 2KB (adjust as needed)
    constexpr size_t BUFFER_SIZE = 2048;
    std::array<char, BUFFER_SIZE> buffer;

    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(buffer.data(), buffer.size(), format, args_copy);
    va_end(args_copy);

    if (len < 0) {
        // Formatting failed – log error with literal format
        swss::Logger::getInstance().write(
            swss::Logger::SWSS_ERROR,
            "Log formatting failed (vsnprintf error)"
        );
        return;
    }

    if (static_cast<size_t>(len) >= buffer.size()) {
        // Truncate gracefully with "..."
        constexpr size_t trunc_len = BUFFER_SIZE - 4;
        buffer[trunc_len] = '.';
        buffer[trunc_len + 1] = '.';
        buffer[trunc_len + 2] = '.';
        buffer[trunc_len + 3] = '\0';
    }

    // Log with LITERAL format string "%s" → satisfies -Wformat-nonliteral
    swss::Logger::getInstance().write(swssLevel, "%s", buffer.data());
}

} // anonymous namespace

// Registration helper
void registerSwssLogger() {
    fib::registerLogCallback(swssLogBridge);
    fib::setLogLevel(fib::LogLevel::INFO); // INFO for production as default
    SWSS_LOG_NOTICE("FIB logging initialized, log level set to %d",
                    static_cast<int>(fib::getLogLevel()));
}

NHGMgr::NHGMgr(RedisPipeline *pipeline, const std::string &nexthopTableName, const std::string &picTableName, bool isStateTable)
   {
    m_rib_nhg_table = new RIBNHGTable(pipeline, nexthopTableName, isStateTable);
    m_sonic_pic_table = new SonicPICContentTable(pipeline, picTableName, isStateTable);
    m_sonic_id_manager.init({SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT, SONIC_NHG_OBJ_TYPE_NHG_NORMAL});
    m_rib_nhg_table->setSonicIDManager(&m_sonic_id_manager);

    // register SWSS logger as the callback for sonic_fib logs
    registerSwssLogger();
}

int NHGMgr::addNHGFull(const NextHopGroupFull &nhg, uint8_t af) {

    // NHGs are immutable per RIB id; replacements must use a new id.
    if (m_rib_nhg_table->isNHGExist(ribID(nhg.id))) {
        SWSS_LOG_WARN("NHG %d already exists, ignore duplicate add", nhg.id);
        return 0;
    }

    SWSS_LOG_NOTICE("Receiving NHG %d, type %d, af %d", nhg.id, nhg.type, af);
    dumpNHGGroupFull(nhg);

    int ret = addNewNHGFull(nhg, af);
    if (ret != 0){
        SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
        return ret;
    }
    return 0;
}

/*
 * process of add new NHG full
 */
int NHGMgr::addNewNHGFull(const NextHopGroupFull &nhg, uint8_t af) {

    int ret = 0;
    ribID id=ribID(nhg.id);
    RIBNHGEntry *entry;
    ret = m_rib_nhg_table->addEntry(nhg, af);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
        return ret;
    }

    entry = m_rib_nhg_table->getEntry(id);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to get NHG entry after add for %d", nhg.id);
        return -1;
    }

    /*
     * Process NHG offload
     */
    if (entry->needCreateSonicObject()) {

        // no sonic nhg object created, allocate a new id
        sonicObjectID sonicId = m_sonic_id_manager.allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        if (sonicId.id == 0) {
            SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
            m_rib_nhg_table->delEntry(nhg.id);
            return -1;
        }
        
        entry->setSonicNHGObjId(sonicId);
        ret = m_rib_nhg_table->writeToDB(entry);
        if (ret != 0) {
            m_rib_nhg_table->delEntry(nhg.id);
            m_sonic_id_manager.freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicId);
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return ret;
        }

        // store the shared sonic nhg object
        if (entry->isSharedSonicNHG()){
            SonicNHGObjectKey key;
            SonicNHGObjectKey::createSonicNormalNHGObjectKey(entry, key);
            m_rib_nhg_table->insertCreatedSharedNHGObject(key, sonicId);
            SWSS_LOG_DEBUG("Create shared sonic NHG for %d, sonic id %d", nhg.id, sonicId.id);
        }else{
            SWSS_LOG_DEBUG("Create sonic NHG for %d, sonic id %d", nhg.id, sonicId.id);
        }
    }

    /*
     * Process the PICContent offload
     */
    if (entry->hasSonicPICObj()) {
        SWSS_LOG_DEBUG("Create sonic PICContent object for NHG %d", nhg.id);
        ret = createSonicPICObject(entry);
        if (ret != 0) {
            // delEntry handles removeFromDB and subSonicNHGObjectRef internally
            m_rib_nhg_table->delEntry(nhg.id);
            SWSS_LOG_ERROR("Failed to create sonic nhg %d", nhg.id);
            return ret;
        }
    }
    return 0;
}

/*
 * Create the sonic PIC Content object from RIB entry
 */
int NHGMgr::createSonicPICObject(RIBNHGEntry *entry) {

    sonicNhgObjType sType = entry->getSonicObjType();
    if (entry->getSonicObjType() != SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT){
        SWSS_LOG_ERROR("Unsupported sonic nhg type %d", sType);
        return -1;
    }

    // srv6 sonic nhg create
    SonicPICContentObject sonicObj;
    sonicObjectID sonicPICContentID;
    int ret = 0;

    // allocate sonic pic object id
    sonicPICContentID = m_sonic_id_manager.allocateID(sType);
    if (sonicPICContentID.id == 0) {
        SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
        return -1;
    }

    ret = entry->createSRv6PICObjFromRIBEntry(sonicObj);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to create sonic PIC obj for %d", entry->getRIBIDNum());
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        return ret;
    }
    sonicObj.sonicID = sonicPICContentID;

    // add the SonicPICContentEntry
    ret = m_sonic_pic_table->addEntry(sonicObj);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add sonic PIC obj for %d", entry->getRIBIDNum());
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        return ret;
    }

    SonicPICContentEntry *sonicEntry = m_sonic_pic_table->getEntry(sType, sonicPICContentID);
    if (sonicEntry == nullptr) {
        SWSS_LOG_ERROR("Failed to get sonic PIC entry for %d", entry->getRIBIDNum());
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        return ret;
    }

    // write to DB
    ret = m_sonic_pic_table->writeToDB(sonicEntry);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", sonicPICContentID.id);
        m_sonic_id_manager.freeID(sType, sonicPICContentID);
        m_sonic_pic_table->delEntry(sType, sonicPICContentID);
        return ret;
    }

    // set the sonic PIC Content id of rib entry
    entry->setSonicPICObjId(sonicPICContentID);
    SWSS_LOG_NOTICE("Create sonic PIC Content object for NHG %d, sonic pic id: %d, type: %d", entry->getRIBID().id, sonicPICContentID.id, entry->getSonicObjType());
    return 0;
}

/*
 * NHG remove entrance for FIB Block
 */
int NHGMgr::delNHGFull(uint32_t id) {

    SWSS_LOG_NOTICE("Del NextHop group id %d", id);
    ribID ribId = ribID(id);

    if (!m_rib_nhg_table->isNHGExist(ribId)) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    RIBNHGEntry *entry = m_rib_nhg_table->getEntry(ribId);
    if (entry == nullptr) {
        return 0;
    }

    // del the sonic PIC Content first
    if (entry->hasSonicPICObj()) {
        sonicObjectID sonicPICContentObjID = entry->getSonicPICObjID();
        SonicPICContentEntry* sonicEntry = m_sonic_pic_table->getEntry(entry->getSonicObjType(), sonicPICContentObjID);
        if (sonicEntry != nullptr){
            m_sonic_pic_table->delEntry(entry->getSonicObjType(), sonicPICContentObjID);
            m_sonic_id_manager.freeID(entry->getSonicObjType(), sonicPICContentObjID);
        }
    }

    m_rib_nhg_table->delEntry(ribId);
    return 0;
}

void NHGMgr::dumpNHGGroupFull(const fib::NextHopGroupFull &nhg) {
    SWSS_LOG_DEBUG("NHG ID %d, type %d, ifname %s", nhg.id, nhg.type, nhg.ifname.c_str());

    if (nhg.type == fib::NEXTHOP_TYPE_IPV4 || nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   gateway %s", gateway);
    }
    if (nhg.type == fib::NEXTHOP_TYPE_IPV6 || nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   gateway %s", gateway);
    }
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        SWSS_LOG_DEBUG("   group member %d, num_direct %d", it->id, it->num_direct);
    }
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {
        SWSS_LOG_DEBUG("   depends member %d", *it);
    }
    for (auto it = nhg.dependents.begin(); it != nhg.dependents.end(); it++) {
        SWSS_LOG_DEBUG("   dependents member %d", *it);
    }
    if (nhg.nh_srv6 != nullptr && nhg.nh_srv6->seg6_segs != nullptr){
        char seg_str[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.nh_srv6->seg6_segs->seg[0], seg_str, INET6_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   srv6 seg6_segs %s", seg_str);
        char seg_src[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.src, seg_src, INET6_ADDRSTRLEN);
        SWSS_LOG_DEBUG("   srv6 seg_src %s", seg_src);
    }
}


/*
 * get RIB NHG entry by RIB ID
 */
RIBNHGEntry *NHGMgr::getRIBNHGEntryByRIBID(uint32_t id) {
    return m_rib_nhg_table->getEntry(ribID(id));
}

/*
 * get Sonic PIC Content entry by Sonic PIC Content ID and type
 */
SonicPICContentEntry *NHGMgr::getSonicPICByID(sonicNhgObjType type, uint32_t id) {
    return m_sonic_pic_table->getEntry(type, sonicObjectID(id));
}

/*
 * check if the sonic NHG id is in used
 */
bool NHGMgr::isSonicNHGIDInUsed(uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicObjectID(id));
}

/*
 * get Sonic PIC Content entry by RIB NHG ID
 */
SonicPICContentEntry *NHGMgr::getSonicPICByRIBID(uint32_t id) {
    RIBNHGEntry *ribnhgEntry = getRIBNHGEntryByRIBID(id);
    if (ribnhgEntry == nullptr) {
        return nullptr;
    }
    if (!ribnhgEntry->hasSonicPICObj()) {
        return nullptr;
    }
    return getSonicPICByID(ribnhgEntry->getSonicObjType(), ribnhgEntry->getSonicPICObjIDNum());;
}

/*
 * check if the sonic PIC Content object id is in used
 */
bool NHGMgr::isSonicPICIDInUsed(sonicNhgObjType type, uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(type, sonicObjectID(id));
}

/*
 * check if the sonic NHG object id is in used
 */
bool SonicIDMgr::isSonicObjIDUsed(sonicNhgObjType type, sonicObjectID id) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator != nullptr) {
        return allocator->isInUsed(id);
    }
    return false;
}

RIBNHGTable::RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable)
    : m_nexthop_groupTable(pipeline, tableName, isStateTable){
}

/*
 * get RIB NHG entry by RIB ID
 */
RIBNHGEntry *RIBNHGTable::getEntry(ribID id) {
    auto it = m_nhg_map.find(id);
    if (it == m_nhg_map.end()) {
        return nullptr;
    }
    return it->second;
}

/*
 * delete RIB NHG entry by RIB ID
 */
void RIBNHGTable::delEntry(ribID ribId) {
    if (m_nhg_map.find(ribId) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", ribId.id);
        return;
    }

    RIBNHGEntry *entry = m_nhg_map[ribId];
    sonicObjectID sonicNHGID = entry->getSonicObjID();

    if (entry->isSharedSonicNHG()){
        this->subSonicNHGObjectRef(entry->getSonicNHGObjectKey());
    }else{
        this->removeFromDB(sonicNHGID);
        m_sonic_id_manager->freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicNHGID);
    }
    delete entry;
    m_nhg_map.erase(ribId);
    return;
}

/*
 * add RIB NHG entry from NextHopGroupFull
 */
int RIBNHGTable::addEntry(const NextHopGroupFull &nhg, uint8_t af) {
    if (m_nhg_map.find(nhg.id) != m_nhg_map.end()) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }
    RIBNHGEntry *entry = RIBNHGEntry::createNHGEntry(this);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }

    int ret = entry->setEntry(nhg, af);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    m_nhg_map.insert(std::make_pair(nhg.id, entry));
    return 0;
}

/*
 * check if NHG entry exists
 */
bool RIBNHGTable::isNHGExist(ribID id) {
    auto it = m_nhg_map.find(id);
    if (it != m_nhg_map.end()) {
        return true;
    }
    return false;
}

/*
 * write Sonic NHG Object to APP_DB
 */
int RIBNHGTable::writeToDB(RIBNHGEntry *entry) {

    std::vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getRIBIDNum());
        return -1;
    }

    m_nexthop_groupTable.set(std::to_string(entry->getSonicObjID().id), fvVector);

    SWSS_LOG_DEBUG("writeToDB NEXTHOP_GROUP_TABLE : RIB_id %d sonic_id %d",
                    entry->getRIBIDNum(), entry->getSonicObjIDNum());
    for (auto it = fvVector.begin(); it != fvVector.end(); it++) {
        SWSS_LOG_DEBUG("  %s %s", fvField(*it).c_str(), fvValue(*it).c_str());
    }

    return 0;
}

/*
 * remove Sonic NHG Object from APP_DB by ID
 */
void RIBNHGTable::removeFromDB(sonicObjectID id) {
    m_nexthop_groupTable.del(std::to_string(id.id));
    return;
}

/*
 * clean all RIB NHG entry in table
 */
void RIBNHGTable::cleanUp() {
    for (auto it = m_nhg_map.begin(); it != m_nhg_map.end(); it++) {
        delete it->second;
    }
    m_nhg_map.clear();
    m_created_shared_nhg_map.clear();
}

void RIBNHGTable::insertCreatedSharedNHGObject(SonicNHGObjectKey key, sonicObjectID id) {
    m_created_shared_nhg_map.insert(std::make_pair(key, SonicNHGObjectInfo(id, 1)));
}

void RIBNHGTable::addSonicNHGObjectRef(SonicNHGObjectKey key) {
    m_created_shared_nhg_map[key].refCount++;
}

void RIBNHGTable::subSonicNHGObjectRef(SonicNHGObjectKey key) {
    if (m_created_shared_nhg_map.find(key) == m_created_shared_nhg_map.end()){
        return ;
    }
    if (m_created_shared_nhg_map[key].refCount == 0) {
        /*
         * Underflow indicates a reference accounting bug. Keep the entry and
         * the sonic object as-is to avoid masking the bug, and fail loudly.
         */
        SWSS_LOG_ERROR("Reference count underflow for shared NHG object, sonic id %d",
                       m_created_shared_nhg_map[key].sonicID.id);
        return;
    }
    (m_created_shared_nhg_map[key].refCount)--;

    /*
     * if refCount is 0, then remove the sonic nhg object
     */
    if(m_created_shared_nhg_map[key].refCount == 0){
        this->removeFromDB(m_created_shared_nhg_map[key].sonicID);
        m_sonic_id_manager->freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, m_created_shared_nhg_map[key].sonicID);
        m_created_shared_nhg_map.erase(key);
    }
}

int RIBNHGTable::getCreatedSharedNHGObjectID(SonicNHGObjectKey key) {
    if (m_created_shared_nhg_map.find(key) == m_created_shared_nhg_map.end()){
        return 0;
    }
    return m_created_shared_nhg_map[key].sonicID.id;
}

RIBNHGEntry::RIBNHGEntry(RIBNHGTable *table) : m_table(table) {
}

RIBNHGEntry::~RIBNHGEntry() {
    m_table = nullptr;
}

/* static creation func */
RIBNHGEntry *RIBNHGEntry::createNHGEntry(RIBNHGTable *mTable) {
    RIBNHGEntry *entry = new RIBNHGEntry(mTable);
    return entry;
}

/*
 * create the corresponding SonicPICContentObject from RIB NHG Entry
 */
int RIBNHGEntry::createSonicPICContentObjectFromRIBEntry(SonicPICContentObject &sonicNhgOut) {
    switch (getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT:
            return createSRv6PICObjFromRIBEntry(sonicNhgOut);
        default:
            SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", getSonicObjType());
            return -1;
    }
}

/*
 * create the corresponding SonicPICContentObject in SRv6 VPN case from RIB NHG Entry
 */
int RIBNHGEntry::createSRv6PICObjFromRIBEntry(SonicPICContentObject &sonicNhgOut) {
    sonicNhgOut.groupMember.clear();
    sonicNhgOut.type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT;
    sonicNhgOut.sonicID = getSonicPICObjID();

    // single hop: Use SRv6 VPN fields, use them directly.
    if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr) {
        sonicNhgOut.nexthop = m_nexthop;
        sonicNhgOut.segSrc = m_segSrc;
        sonicNhgOut.vpnSid = m_vpnSid;
        sonicNhgOut.ifName = m_ifName;
        return 0;
    }

    // Multi-hop: Use weight from m_group map.
    for (auto member: m_resolvedGroup) {
        RIBNHGEntry *memberEntry = m_table->getEntry(member.first.id);
        if (memberEntry == nullptr) {
            SWSS_LOG_ERROR("RIBNHGEntry is not exist: %d", member.first.id);
            return -1;
        }
        if (memberEntry->hasSonicPICObj() && memberEntry->getSonicObjType() == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
            if (memberEntry->getSonicPICObjIDNum() != 0) {
                sonicNhgOut.groupMember.insert(std::make_pair(memberEntry->getSonicPICObjID(), member.second));
            }
        }
    }

    return 0;
}

/* set an RIB Entry from a nexthopgroupfull */
int RIBNHGEntry::setEntry(const NextHopGroupFull &nhg, uint8_t af) {
    m_rib_id = nhg.id;
    m_nhg = nhg;
    m_af = af;
    m_group.clear();
    m_depends.clear();
    m_dependents.clear();
    m_resolvedGroup.clear();
    m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    m_has_sonic_pic_obj = false;
    m_is_srv6_nhg = false;
    m_is_shared_sonic_nhg = false;
    m_create_sonic_nhg_obj = false;

    // update m_depends set
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {
        m_depends.insert(*it);
        SWSS_LOG_DEBUG("NextHop id %d add depends %d.", m_rib_id.id, *it);
    }

    // update m_dependents set
    for (auto it = nhg.dependents.begin(); it != nhg.dependents.end(); it++) {
        m_dependents.insert(*it);
        SWSS_LOG_DEBUG("NextHop id %d add dependents %d.", m_rib_id.id, *it);
    }

    // check the full list NHG entry and update m_group set
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        // validate group member
        if (!m_table->isNHGExist(it->id)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", it->id);
            return -1;
        }
        m_group.insert(std::make_pair(it->id, it->weight));
        SWSS_LOG_DEBUG("NextHop id %d add group %d.", m_rib_id.id, it->id);
    }

    /*
     * check if we need to create sonic PIC Content obj
     * and sonic pic obj type
     * */
    checkNeedCreateSonicPICObj();

    /* get resolved group from nhg full */
    if(getResolvedGroupFromNHGFull()!=0){
        SWSS_LOG_ERROR("Failed to get resolved group from nhg full");
        return -1;
    }

    // sync fv vector
    if (this->syncFvVector() != 0) {
        SWSS_LOG_ERROR("Failed to sync fv vector for %d", nhg.id);
        return -1;
    }

    /*
     * check if we need to create sonic nhg obj, if already exists, then set the sonic obj id for this entry
     */
    checkNeedCreateSonicNHGObj();

    return 0;
}

/* sync fvVector from entry */
int RIBNHGEntry::syncFvVector() {
    m_fvVector.clear();
    // get the FV vector fields
    if (getNHGFields() != 0) {
        SWSS_LOG_ERROR("get nhg fields failed");
        return -1;
    }

    // check the nexthop str
    if (m_nexthop.empty() && m_nhg.nh_grp_full_list.size() > 0 && m_nhg.type != fib::NEXTHOP_TYPE_IFINDEX) {
        SWSS_LOG_ERROR("nexthop is empty");
        return -1;
    }
    if (m_nexthop.empty()) {
        if (m_af == AF_INET) {
            m_nexthop = "0.0.0.0";
        } else if (m_af == AF_INET6) {
            m_nexthop = "::";
        } else {
            SWSS_LOG_ERROR("sync fv vector failed, unsupported address family %d", m_af);
            return -1;
        }
    }

    FieldValueTuple nh("nexthop", m_nexthop.c_str());
    m_fvVector.push_back(nh);

    if (!m_ifName.empty()) {
        FieldValueTuple ifname("ifname", m_ifName.c_str());
        m_fvVector.push_back(ifname);
    }
    if (!m_weight.empty()) {
        FieldValueTuple wg("weight", m_weight.c_str());
        m_fvVector.push_back(wg);
    }
    if (!m_segSrc.empty() && m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
        FieldValueTuple wg("seg_src", m_segSrc.c_str());
        m_fvVector.push_back(wg);
    }

    SWSS_LOG_DEBUG("NextHopGroup table set: nexthop[%s] ifname[%s] weight[%s] seg_src[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                  m_weight.c_str(), m_segSrc.c_str());
    return 0;
}

/* get fields from entry */
int RIBNHGEntry::getNHGFields() {

    if (m_resolvedGroup.size() > 0) {
        SWSS_LOG_DEBUG("multi nexthop group");
        // nexthop with group member
        return getNextHopGroupFields();

    } else {
        // nexthop without group member
        SWSS_LOG_DEBUG("single nexthop group");
        return getNextHopFields();
    }
}

/*
 * get FV vector fields and sonic pic Objects fields from multi nexthopgroup entry
 *
 * below fields of entry will be set:
 *      m_nexthop
 *      m_ifName
 *      m_weight
 *
 * for SRv6 VPN case:
 *      m_vpnSid
 *      m_segSrc
 */
int RIBNHGEntry::getNextHopGroupFields() {
    string nexthops = "";
    string ifnames = "";
    string weights = "";
    string vpnSids = "";
    string segSrcs = "";
    for (const auto &nh: m_resolvedGroup) {
        ribID ribId(nh.first.id);
        string weight = to_string(nh.second.weight);
        if (!m_table->isNHGExist(ribId)) {
            SWSS_LOG_ERROR("NextHop group is incomplete: nhg %d not found", ribId.id);
            return -1;
        }
        RIBNHGEntry *entry = this->m_table->getEntry(ribId);
        if (!nexthops.empty()) {
            nexthops += NHG_DELIMITER;
        }
        if (!ifnames.empty()) {
            ifnames += NHG_DELIMITER;
        }
        if (!weights.empty()) {
            weights += NHG_DELIMITER;
        }
        nexthops += entry->getNextHopStr();
        SWSS_LOG_DEBUG(" entry nexthop: [%s]", entry->getNextHopStr().c_str());
        ifnames += entry->getInterfaceNameStr();
        SWSS_LOG_DEBUG(" entry interface: [%s]", entry->getInterfaceNameStr().c_str());
        weights += weight;
        SWSS_LOG_DEBUG(" entry weight: [%s]", weight.c_str());

        /* SRv6 VPN SID */
        if(m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT){
            if (!vpnSids.empty()){
                vpnSids += NHG_DELIMITER;
            }
            if(!segSrcs.empty()){
                segSrcs += NHG_DELIMITER;
            }
            vpnSids += entry->getVPNSIDStr();
            segSrcs += entry->getSegSrcStr();
            SWSS_LOG_DEBUG(" entry vpnSid: [%s], segSrc: [%s]", vpnSids.c_str(), segSrcs.c_str());
        }
    }
    m_nexthop = nexthops;
    m_ifName = ifnames;
    m_vpnSid = vpnSids;
    m_segSrc = segSrcs;
    m_weight = weights;
    SWSS_LOG_DEBUG("get NextHopGroup fields done, nexthop[%s] ifname[%s] weight[%s] vpnSid[%s] segSrc[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                    m_weight.c_str(), m_vpnSid.c_str(), m_segSrc.c_str());
    return 0;
}

/*
 * get FV vector fields and sonic pic Objects fields from multi nexthopgroup entry
 *
 * below fields of entry will be set:
 *      m_nexthop
 *      m_ifName
 *
 * for SRv6 VPN case:
 *      m_vpnSid
 *      m_segSrc
 */
int RIBNHGEntry::getNextHopFields() {

    if (m_af == AF_INET) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &m_nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        m_nexthop = gateway;
        m_ifName = m_nhg.ifname;
    }else if(m_af == AF_INET6) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &m_nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        m_nexthop = gateway;
        m_ifName = m_nhg.ifname;
    }

    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
        char sid[INET6_ADDRSTRLEN] = {0};
        char seg_src[INET6_ADDRSTRLEN] = {0};
        if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr){
            inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_segs->seg[0], sid, INET6_ADDRSTRLEN);
            m_vpnSid = sid;
            inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_src, seg_src, INET6_ADDRSTRLEN);
            m_segSrc = seg_src;
        }else{
            SWSS_LOG_ERROR("single nexthop id %d type srv6 gateway has no seg6_segs", m_rib_id.id);
            return -1;
        }
    }
    SWSS_LOG_DEBUG("get NextHopGroup fields done, nexthop[%s] ifname[%s] weight[%s] vpnSid[%s] segSrc[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                    m_weight.c_str(), m_vpnSid.c_str(), m_segSrc.c_str());
    return 0;
}

/*
 * get resolved group from nexthopgroupfull
 * below fields of entry will be set:
 *      m_resolvedGroup
 *      m_is_single
 */
int RIBNHGEntry::getResolvedGroupFromNHGFull() {
    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_NORMAL) {
        for (auto nhg: m_nhg.nh_grp_full_list) {
            if (nhg.num_direct == 0) {
                m_resolvedGroup.insert(std::make_pair(nhg.id, nhg.weight));
                SWSS_LOG_DEBUG("NextHop id %d add resolved group %d.", m_rib_id.id, nhg.id);
            }
        }

    } else if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
        for (auto nhg: m_nhg.nh_grp_full_list){
            RIBNHGEntry *entry = m_table->getEntry(nhg.id);
            if (entry == nullptr){
                SWSS_LOG_ERROR("NextHop id %d in group not found.", nhg.id);
                return -1;
            }
            if (entry->hasSonicPICObj()){
                m_resolvedGroup.insert(std::make_pair(nhg.id, nhg.weight));
                SWSS_LOG_DEBUG("NextHop id %d add resolved group %d, type %d.", m_rib_id.id, nhg.id, entry->getSonicObjType());
            }
        }
    }
    if(m_resolvedGroup.size() <= 1){
        m_is_single = true;
    }else{
        m_is_single = false;
    }
    return 0;
}

/*
 * check if we need to create sonic pic obj and the object type
 * below fields of entry will be set:
 *      m_sonic_obj_type
 *      m_has_sonic_pic_obj
 */
void RIBNHGEntry::checkNeedCreateSonicPICObj() {

    /* now only for srv6 vpn PIC Content obj */

    /* for srv6 gateway, check if we need to create pic group obj */
    for (auto it = m_group.begin(); it != m_group.end(); it++) {
        RIBNHGEntry *entry = m_table->getEntry(ribID(it->first.id));
        if (entry == nullptr) {
            continue;
        }
        if (entry->hasSonicPICObj()) {
            m_sonic_obj_type = entry->getSonicObjType();
            m_has_sonic_pic_obj = true;
            m_is_shared_sonic_nhg = true;
            m_is_srv6_nhg = entry->isSRv6Nhg();
            SWSS_LOG_DEBUG("NextHop id %d has sonic pic obj, is multi nexthop group.", it->first.id);
            return;
        }
        if(entry->isSRv6Nhg()) {
            m_is_srv6_nhg = true;
        }
    }

    if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr) {
        m_is_srv6_nhg = true;
    }

    /*
     * For srv6 received nexthop, should create a sonic pic obj and shared sonic NHG object.
     * This covers both single-hop SRv6 VPN nexthops (no group members) and
     * recursive SRv6 VPN nexthops that carry their own SRv6 VPN info.
     */
    if (m_is_srv6_nhg && CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FROM_EXTERNAL)) {
        m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT;
        m_has_sonic_pic_obj = true;
        m_is_shared_sonic_nhg = true;
        SWSS_LOG_DEBUG("NextHop id %d has sonic pic obj.", m_rib_id.id);
        return;
    }

    return;
}


void RIBNHGEntry::checkNeedCreateSonicNHGObj() {
    SonicNHGObjectKey::createSonicNormalNHGObjectKey(this, m_sonic_nhg_key);

    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT){
        /*
         * For SRv6 VPN NHG, create shared NHG object.
         */
        sonicObjectID id = m_table->getCreatedSharedNHGObjectID(m_sonic_nhg_key);
        if (id == 0){
            m_create_sonic_nhg_obj = true;
        }else{
            m_create_sonic_nhg_obj = false;
            if (m_sonic_obj_id != id) {
                m_sonic_obj_id = id;
                m_table->addSonicNHGObjectRef(m_sonic_nhg_key);
            }
        }
    }else{
        /*
         * Skipped received NHG without SRv6 info.
         */
        if (CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FROM_EXTERNAL)){
            SWSS_LOG_DEBUG("NextHop %d is a received NHG without SRv6 info, skip create sonic object.", m_rib_id.id);
            m_create_sonic_nhg_obj = false;
            return ;
        }

        /*
         * Skipped NHG with SRv6 info but not received NHG.
         */
        if (!CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FROM_EXTERNAL) && m_is_srv6_nhg){
            SWSS_LOG_DEBUG("NextHop %d is a NHG with SRv6 VPN, but not is received NHG, skip create sonic object.", m_rib_id.id);
            m_create_sonic_nhg_obj = false;
            return ;
        }
        if (m_is_single == true){
            m_create_sonic_nhg_obj = false;
        } else{
            m_create_sonic_nhg_obj = true;
        }
    }
}


SonicPICContentTable::SonicPICContentTable(RedisPipeline *pipeline, const std::string &picTableName, bool isStateTable)
    : m_pic_contextTable(pipeline, picTableName, isStateTable){
}

/*
 * write the sonic pic object into DB
 * SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT into PIC_CONTEXT_TABLE
 */
int SonicPICContentTable::writeToDB(SonicPICContentEntry *entry) {
    std::vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getSonicPicContentObjIdNum());
        return -1;
    }
    if (entry->getType() == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
        m_pic_contextTable.set(std::to_string(entry->getSonicPicContentObjIdNum()), fvVector);
    }
    return 0;
}

/*
 * remove the sonic pic object from DB
 */
void SonicPICContentTable::removeFromDB(SonicPICContentEntry *entry) {
    if (entry->getType() == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
        m_pic_contextTable.del(std::to_string(entry->getSonicPicContentObjIdNum()));
        SWSS_LOG_DEBUG("Remove PIC Context object id %d", entry->getSonicPicContentObjIdNum());
    }
    return;
}

/*
 * add the SonicPICContentEntry
 */
int SonicPICContentTable::addEntry(const SonicPICContentObject &sonicObj) {
    if (getEntry(sonicObj.type, sonicObj.sonicID) != nullptr) {
        SWSS_LOG_ERROR("SonicPICContentObject is already exist: %d", sonicObj.sonicID.id);
        return -1;
    }

    SonicPICContentEntry *entry = SonicPICContentEntry::createSonicPICContentEntry(this);
    if (entry == nullptr) {
        return -1;
    }

    // set SonicPICContentEntry fields
    int ret = entry->setEntry(sonicObj);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    switch (entry->getType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT: {
            m_pic_map[entry->getSonicPicContentObjId()] = entry;
            break;
        }
        default: {
            SWSS_LOG_WARN("Skip SonicPICContentObject type");
            break;
        }
    }

    return 0;
}


/*
 * delete the SonicPICContentEntry by type and id
 */
void SonicPICContentTable::delEntry(sonicNhgObjType type, sonicObjectID sonicID) {
    SonicPICContentEntry *entry = nullptr;
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT: {
            if (m_pic_map.find(sonicID) != m_pic_map.end()) {
                entry = m_pic_map[sonicID];
                m_pic_map.erase(sonicID);
            } else {
                entry = nullptr;
            }
            break;
        }
        default: {
            entry = nullptr;
            break;
        }
    }
    if (entry == nullptr) {
        SWSS_LOG_WARN("SonicPICContentObject is not exist: %d %d", type, sonicID.id);
        return;
    }

    removeFromDB(entry);
    delete entry;
    return;
}

/*
 * delete all the SonicPICContentEntry
 */
void SonicPICContentTable::cleanUp() {
    for (auto it = m_pic_map.begin(); it != m_pic_map.end(); it++) {
        delete it->second;
    }
    m_pic_map.clear();
}


/*
 * get the SonicPICContentEntry by type and id
 */
SonicPICContentEntry *SonicPICContentTable::getEntry(sonicNhgObjType type, sonicObjectID sonicID) {
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT: {
            if (m_pic_map.find(sonicID) != m_pic_map.end()) {
                return m_pic_map[sonicID];
            }
            return nullptr;
        }
        default: {
            SWSS_LOG_ERROR("Failed to get SonicPICContentEntry: %d %d", type, sonicID.id);
            return nullptr;
        }
    }
}

/*
 * create SonicPICContentEntry
 */
SonicPICContentEntry *SonicPICContentEntry::createSonicPICContentEntry(SonicPICContentTable *mTable) {
    SonicPICContentEntry *entry = new SonicPICContentEntry(mTable);
    return entry;
}


/*
 * sync the fvVector for SonicPICContentEntry
 */
int SonicPICContentEntry::syncFvVector() {
    m_fvVector.clear();
    switch (m_sonic_pic_obj.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT: {
            return syncFvVectorForSRv6PIC();
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", m_sonic_pic_obj.type);
            return -1;
        }
    }
}

/*
 * sync the fvVector for SRv6 Gateway
 */
int SonicPICContentEntry::syncFvVectorForSRv6PIC() {
    if (m_sonic_pic_obj.groupMember.size() == 0) {
        m_fvVector.push_back(FieldValueTuple("nexthop", m_sonic_pic_obj.nexthop));
        m_fvVector.push_back(FieldValueTuple("ifname", m_sonic_pic_obj.ifName));
        m_fvVector.push_back(FieldValueTuple("seg_src", m_sonic_pic_obj.segSrc));
        m_fvVector.push_back(FieldValueTuple("vpn_sid", m_sonic_pic_obj.vpnSid));
    }else {
        string nexthops = "";
        string vpnSids = "";
        string ifnames = "";
        string weights = "";
        string segSrcs = "";
        for (auto member: m_group) {
            SonicPICContentEntry *entry = m_table->getEntry(SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT, sonicObjectID(member.first.id));
            if (entry == nullptr) {
                SWSS_LOG_ERROR("SonicPICContentObject is not exist: %d", member.first.id);
                return -1;
            }

            SonicPICContentObject obj = entry->getObj();
            if (obj.groupMember.size() > 0) {
                SWSS_LOG_ERROR("SonicPICContentObject member is a group: %d", member.first.id);
                return -1;
            }

            if (!nexthops.empty()) {
                nexthops += NHG_DELIMITER;
            }
            nexthops += obj.nexthop;

            if (!ifnames.empty()) {
                ifnames += NHG_DELIMITER;
            }
            if (!obj.ifName.empty()) {
                ifnames += obj.ifName;
            } else {
                ifnames += "unknown";
            }

            if (!vpnSids.empty()) {
                vpnSids += NHG_DELIMITER;
            }
            vpnSids += obj.vpnSid;

            if (!weights.empty()) {
                weights += NHG_DELIMITER;
            }
            weights += std::to_string(member.second.weight);

            if (!segSrcs.empty()) {
                segSrcs += NHG_DELIMITER;
            }
            segSrcs += obj.segSrc;
        }
        m_fvVector.push_back(FieldValueTuple("nexthop", nexthops));
        m_fvVector.push_back(FieldValueTuple("ifname", ifnames));
        m_fvVector.push_back(FieldValueTuple("weight", weights));
        m_fvVector.push_back(FieldValueTuple("vpn_sid", vpnSids));
        m_fvVector.push_back(FieldValueTuple("seg_src", segSrcs));
    }

    for (auto it = m_fvVector.begin(); it != m_fvVector.end(); it++) {
        SWSS_LOG_DEBUG("Sync fvVector for SonicPICContentEntry %d, type %d, %s %s", m_sonic_obj_id.id, m_sonic_pic_obj.type, fvField(*it).c_str(), fvValue(*it).c_str());
    }
    return 0;
}

int SonicPICContentEntry::setEntry(const SonicPICContentObject &obj) {
    switch (obj.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT: {
            int ret = setSRv6PICEntry(obj);
            if (ret != 0) {
                return ret;
            }
            break;
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", obj.type);
            return -1;
        }
    }
    int ret = syncFvVector();
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for entry");
        return ret;
    }
    return 0;
}

int SonicPICContentEntry::setSRv6PICEntry(const SonicPICContentObject &obj) {
    m_sonic_pic_obj = obj;
    m_sonic_obj_id = obj.sonicID;
    m_sonic_obj_key = SonicNHGObjectKey::createSonicPICContentObjectKey(obj);
    m_group.clear();
    for (auto member: obj.groupMember) {
        SonicPICContentEntry *entry = m_table->getEntry(obj.type, sonicObjectID(member.first.id));
        if (entry == nullptr) {
            SWSS_LOG_ERROR("SonicPICContentObject is not exist: %d", member.first.id);
            return -1;
        }
        m_group.insert(std::make_pair(entry->getSonicPicContentObjId(), member.second));
    }
    return 0;
}

SonicPICContentEntry::SonicPICContentEntry(SonicPICContentTable *mTable) : m_table(mTable) {
}

SonicPICContentEntry::~SonicPICContentEntry() {
    m_table = nullptr;
}

sonicObjectID SonicIDMgr::allocateID(sonicNhgObjType type) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator == nullptr) {
        SWSS_LOG_ERROR("SonicIDAllocator is not exist: %d", type);
        return 0;
    }
    return allocator->allocateID();
}

void SonicIDMgr::freeID(sonicNhgObjType type, sonicObjectID id) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator == nullptr) {
        SWSS_LOG_ERROR("SonicIDAllocator is not exist: %d", type);
        return;
    }
    allocator->freeID(id);
}

SonicIDAllocator *SonicIDMgr::getAllocator(sonicNhgObjType type) {
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT:
            return m_pic_id_allocator;
        case SONIC_NHG_OBJ_TYPE_NHG_NORMAL:
            return m_nhg_id_allocator;
        default:
            return nullptr;
    }
}

int SonicIDMgr::init(std::vector<sonicNhgObjType> supportedObjs) {
    for (auto type: supportedObjs) {
        switch (type) {
            case SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT: {
                if (m_pic_id_allocator == nullptr) {
                    m_pic_id_allocator = new SonicIDAllocator(APP_PIC_CONTEXT_TABLE_NAME);
                }
                break;
            }
            case SONIC_NHG_OBJ_TYPE_NHG_NORMAL: {
                if (m_nhg_id_allocator == nullptr) {
                    m_nhg_id_allocator = new SonicIDAllocator(APP_NEXTHOP_GROUP_TABLE_NAME);
                }
                break;
            }
            default: {
                SWSS_LOG_ERROR("Unsupported SonicPICContentObject type: %d", type);
                return -1;
            }
        }
    }
    return 0;
}

sonicObjectID SonicIDAllocator::allocateID() {
    uint32_t attempts = 0;
    while (m_id_map.find(g_id) != m_id_map.end()) {
        if (++attempts >= UINT32_MAX) {
            SWSS_LOG_ERROR("ID space exhausted");
            return 0;
        }
        if (g_id == 0) {
            g_id = 1;
        } else {
            g_id++;
        }
    }
    m_id_map[g_id] = 1;
    sonicObjectID allocated = sonicObjectID(g_id);
    g_id++;
    if (g_id == 0) {
        g_id = 1;
    }
    return allocated;
};

void SonicIDAllocator::freeID(sonicObjectID id) {
    if (m_id_map.find(id) != m_id_map.end()) {
        m_id_map.erase(id);
    }
}

bool SonicIDAllocator::isInUsed(sonicObjectID id) {
    if (m_id_map.find(id) != m_id_map.end()) {
        return true;
    }
    return false;
}

/*
 * create SonicNHGObjectKey from SonicPICContentObject
 */
SonicNHGObjectKey SonicNHGObjectKey::createSonicPICContentObjectKey(SonicPICContentObject obj) {
    SonicNHGObjectKey key;
    key.groupMember = obj.groupMember;
    key.type = obj.type;
    if (obj.type == SONIC_NHG_OBJ_TYPE_NHG_WITH_SRV6_PIC_CONTEXT) {
        key.nexthop = obj.nexthop;
        key.segSrc = obj.segSrc;
        key.vpnSid = obj.vpnSid;
        key.ifName = obj.ifName;
    }
    return key;
}

void SonicNHGObjectKey::createSonicNormalNHGObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out) {
    key_out.type = SONIC_NHG_OBJ_TYPE_NHG_NORMAL;
    key_out.groupMember.clear();
    key_out.nexthop="";
    key_out.ifName="";
    key_out.segSrc="";
    key_out.vpnSid="";
    if (entry->isSingleNexthop()){
        key_out.nexthop = entry->getNextHopStr();
        key_out.ifName = entry->getInterfaceNameStr();
        key_out.segSrc = entry->getSegSrcStr();
        key_out.vpnSid = entry->getVPNSIDStr();
    }else{
        for (auto member: entry->getResolvedGroup()){
            key_out.groupMember.insert(std::make_pair(member.first, member.second));
        }
    }
}

/*
 * create SonicNHGObjectKey from RIBNHGEntry
 */
int SonicNHGObjectKey::createSonicPICContentObjectKey(RIBNHGEntry *entry, SonicNHGObjectKey &key_out) {
    key_out.groupMember.clear();
    SonicPICContentObject obj;
    int ret = entry->createSonicPICContentObjectFromRIBEntry(obj);
    if (ret != 0) {
        return ret;
    }
    key_out = createSonicPICContentObjectKey(obj);
    return 0;
}
