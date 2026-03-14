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

    // TODO FIXME Force all to NOTICE for developing. Remove this line after log level mapping is verified.
    swssLevel = swss::Logger::SWSS_NOTICE;

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
    fib::setLogLevel(fib::LogLevel::DEBUG); // Or INFO for production as default
    SWSS_LOG_NOTICE("FIB logging initialized, log level set to %d",
                    static_cast<int>(fib::getLogLevel()));
}
/*
 * Function used to compare two NextHopGroupFull is equal or not
 * Not include nh_grp_full_list and srv6 fields
 */
static bool compareDependsAndDependents(const NextHopGroupFull *newNHG, const NextHopGroupFull *oldNHG) {
    if ((newNHG->depends.size()) != (oldNHG->depends.size())) {
        return false;
    }
    if ((newNHG->dependents.size()) != (oldNHG->dependents.size())) {
        return false;
    }
    set<int> newNhgDepends;
    set<int> oldNhgDepends;
    set<int> newNhgDependents;
    set<int> oldNhgDependents;
    for (auto it = newNHG->depends.begin(); it != newNHG->depends.end(); it++) {
        newNhgDepends.insert(*it);
    }
    for (auto it = newNHG->dependents.begin(); it != newNHG->dependents.end(); it++) {
        newNhgDependents.insert(*it);
    }
    for (auto it = oldNHG->depends.begin(); it != oldNHG->depends.end(); it++) {
        oldNhgDepends.insert(*it);
        if (newNhgDepends
                    .find(*it) == newNhgDepends.end()) {
            return false;
        }
    }
    for (auto it = oldNHG->dependents.begin(); it != oldNHG->dependents.end(); it++) {
        oldNhgDependents.insert(*it);
        if (newNhgDependents
                    .find(*it) == newNhgDependents.end()) {
            return false;
        }
    }
    for (auto it = newNHG->depends.begin(); it != newNHG->depends.end(); it++) {
        if (oldNhgDepends
                    .find(*it) == oldNhgDepends.end()) {
            return false;
        }
    }
    for (auto it = newNHG->dependents.begin(); it != newNHG->dependents.end(); it++) {
        if (oldNhgDependents
                    .find(*it) == oldNhgDependents.end()) {
            return false;
        }
    }
    return true;
}

/*
 * Function used to compare nh_grp_full_list in NextHopGroupFull is equal or not
 */
static bool compareNHGFullList(const NextHopGroupFull *newNHG, const NextHopGroupFull *oldNHG) {
    if ((newNHG->nh_grp_full_list.size()) != (oldNHG->nh_grp_full_list.size())) {
        return false;
    }
    for (size_t i = 0; i < newNHG->nh_grp_full_list.size(); i++){
        if (newNHG->nh_grp_full_list[i].id != oldNHG->nh_grp_full_list[i].id){
            return false;
        }
        if (newNHG->nh_grp_full_list[i].weight != oldNHG->nh_grp_full_list[i].weight){
            return false;
        }
        if (newNHG->nh_grp_full_list[i].num_direct != oldNHG->nh_grp_full_list[i].num_direct){
            return false;
        }
    }

    return true;
}

/*
 * Function used to get the remove and add set of NHG dependency
 */
static void diffDependency(set<uint32_t> oldSet, set<uint32_t> newSet, set<uint32_t> &outAddSet,
                                 set<uint32_t> &outRemoveSet) {
    for (auto it = oldSet.begin(); it != oldSet.end(); it++) {
        if (newSet.find(*it) == newSet.end()) {
            outRemoveSet.insert(*it);
        }
    }
    for (auto it = newSet.begin(); it != newSet.end(); it++) {
        if (oldSet.find(*it) == oldSet.end()) {
            outAddSet.insert(*it);
        }
    }
}

/*
 * Function used to compare nh_srv6 in NextHopGroupFull is equal or not
 */
static bool compareNHGSRv6Fields(const NextHopGroupFull *new_nhg, const NextHopGroupFull *oldNHG) {
    if (new_nhg->nh_srv6 == nullptr && oldNHG->nh_srv6 == nullptr) {
        return true;
    }
    if (new_nhg->nh_srv6 == nullptr || oldNHG->nh_srv6 == nullptr){
        return false;
    }

    // Compare seg6_segs (vpn_sid)
    bool new_has_segs = (new_nhg->nh_srv6->seg6_segs != nullptr);
    bool old_has_segs = (oldNHG->nh_srv6->seg6_segs != nullptr);
    if (new_has_segs != old_has_segs) {
        return false;
    }
    if (new_has_segs && old_has_segs) {
        if (new_nhg->nh_srv6->seg6_segs->num_segs != oldNHG->nh_srv6->seg6_segs->num_segs) {
            return false;
        }
        for (uint8_t i = 0; i < new_nhg->nh_srv6->seg6_segs->num_segs; i++) {
            if (memcmp(&new_nhg->nh_srv6->seg6_segs->seg[i],
                       &oldNHG->nh_srv6->seg6_segs->seg[i],
                       sizeof(struct in6_addr)) != 0) {
                return false;
            }
        }
    }

    return true;
}

NHGMgr::NHGMgr(RedisPipeline *pipeline, const std::string &nexthopTableName, const std::string &picTableName, bool isStateTable) {
    m_rib_nhg_table = new RIBNHGTable(pipeline, nexthopTableName, isStateTable);
    m_sonic_nhg_table = new SonicNHGTable(pipeline, picTableName, isStateTable);
    m_sonic_id_manager.init({SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, SONIC_NHG_OBJ_TYPE_NHG_NORMAL});

    // register SWSS logger as the callback for sonic_fib logs
    registerSwssLogger();
}

/*
 * NHG add entrance for FIB Block
 */
int NHGMgr::addNHGFull(NextHopGroupFull nhg, uint8_t af) {

    SWSS_LOG_NOTICE("Receiving NHG %d, type %d, af %d", nhg.id, nhg.type, af);
    dumpNHGGroupFull(nhg);

    int ret = 0;
    if (m_rib_nhg_table->isNHGExist(nhg.id)) {
        SWSS_LOG_NOTICE("NHG %d already exists, need to update", nhg.id);
        ret = updateExistingNHGFull(nhg, af);
        if (ret != 0){
            SWSS_LOG_ERROR("Failed to update NHG %d", nhg.id);
            return ret;
        }
    } else {
        SWSS_LOG_NOTICE("NHG %d not found, create new entry", nhg.id);
        ret = addNewNHGFull(nhg, af);
        if (ret != 0){
            SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
            return ret;
        }
    }
    return 0;
}

/*
 * process of add new NHG full
 */
int NHGMgr::addNewNHGFull(NextHopGroupFull nhg, uint8_t af) {

    int ret = 0;
    RIBNHGEntry *entry;
    ret = m_rib_nhg_table->addEntry(nhg, af);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
        return ret;
    }

    entry = m_rib_nhg_table->getEntry(nhg.id);
    if (entry == nullptr) {
        return ret;
    }

    /*
     * Only write the multi nhg entry to DB
     */
    if (entry->isEntryNeedOffload()) {
        uint32_t sonicId = m_sonic_id_manager.allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
        if (sonicId == 0) {
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
        SWSS_LOG_NOTICE("Create sonic NHG for %d, sonic id %d", nhg.id, sonicId);
    }

    /*
     * Create Sonic gateway objet if needed
     */
    if (entry->hasSonicGatewayObj()) {
        SWSS_LOG_NOTICE("Create sonic gateway object for NHG %d", nhg.id);
        SonicGateWayNHGObjectKey key;
        ret = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(entry, key);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to create sonic nhg object key for %d", nhg.id);
            return ret;
        }
        if (m_sonic_nhg_table->getEntry(key) != nullptr) {
            SWSS_LOG_WARN("Sonic NHG Object with key  already exists");
            return 0;
        }
        ret = createSonicGatewayNHGObject(entry);
        if (ret != 0) {
            m_rib_nhg_table->removeFromDB(entry);
            m_rib_nhg_table->delEntry(nhg.id);
            SWSS_LOG_ERROR("Failed to create sonic nhg %d", nhg.id);
            return ret;
        }
    }
    return 0;
}

/*
 * process of update NHG full
 */
int NHGMgr::updateExistingNHGFull(NextHopGroupFull nhg, uint8_t af) {

    RIBNHGEntry *entry;
    int ret = 0;
    entry = m_rib_nhg_table->getEntry(nhg.id);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to get NHG %d when updateNHGFull", nhg.id);
        return -1;
    }

    bool hasPreviousSonicObj = entry->hasSonicGatewayObj();
    uint32_t previousSonicGatewayObjID = entry->getSonicGatewayObjID();
    sonicNhgObjType previousType = entry->getSonicObjType();
    SonicGateWayNHGObjectKey previousKey;
    if (hasPreviousSonicObj) {
        SonicGateWayNHGEntry* sonicGatewayEntry = m_sonic_nhg_table->getEntry(previousType, previousSonicGatewayObjID);
        if (sonicGatewayEntry == nullptr) {
            SWSS_LOG_ERROR("SonicGateWayNHGEntry is not exist: %d", previousSonicGatewayObjID);
            return -1;
        }
        previousKey = sonicGatewayEntry->getSonicGateWayObjKey();

    }
    bool updated = false;

    // check if NHG fields updated and update the rib entry
    ret = m_rib_nhg_table->updateEntry(nhg, af, updated);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to update NHG %d", nhg.id);
        return ret;
    }

    // if NHG fields updated, update into DB
    if (updated) {
        entry = m_rib_nhg_table->getEntry(nhg.id);
        if (entry == nullptr) {
            SWSS_LOG_ERROR("Failed to get NHG after update %d", nhg.id);
            return -1;
        }
        ret = m_rib_nhg_table->writeToDB(entry);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return ret;
        }
    }

    // check if sonic gateway nhg object updated
    // delete previous sonic gateway nhg object if now not have or type changed
    if ((!entry->hasSonicGatewayObj() && hasPreviousSonicObj ) || (entry->getSonicObjType() != previousType)) {
        m_sonic_nhg_table->delEntry(previousType, previousSonicGatewayObjID);
        m_sonic_id_manager.freeID(previousType, previousSonicGatewayObjID);
        entry->setSonicGatewayObjId(0);
    }

    // update the sonic gateway nhg object if updated
    if ((entry->hasSonicGatewayObj() && hasPreviousSonicObj && entry->getSonicObjType() == previousType)) {
        SonicGateWayNHGObjectKey newKey;
        ret = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(entry, newKey);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to create sonic gateway nhg object key for %d", nhg.id);
            return ret;
        }
        if (newKey != previousKey) {
            ret = updateSonicGatewayNHGObject(entry, previousSonicGatewayObjID);
            if (ret != 0) {
                SWSS_LOG_ERROR("Failed to update sonic gateway nhg object for %d", nhg.id);
                return ret;
            }
        }
    }

    // create the sonic gateway nhg object if not exist or type changed
    if ((entry->hasSonicGatewayObj() && !hasPreviousSonicObj) || (entry->getSonicObjType() != previousType)){
        SonicGateWayNHGObjectKey key;
        ret = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(entry, key);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to create sonic nhg object key for %d", nhg.id);
            return ret;
        }
        if (m_sonic_nhg_table->getEntry(key) != nullptr) {
            SWSS_LOG_WARN("Sonic NHG Object with key already exists");
            return 0;
        }
        ret = createSonicGatewayNHGObject(entry);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to create new sonic gateway object for nhg %d", nhg.id);
            return ret;
        }
    }
    return 0;
}

/*
 * Create the sonic gateway nhg object from RIB entry
 */
int NHGMgr::createSonicGatewayNHGObject(RIBNHGEntry *entry) {
    // srv6 sonic nhg create
    SonicGateWayNHGObject sonicObj;
    uint32_t sonicGatewayNHGID;
    int ret = 0;
    sonicNhgObjType sType = entry->getSonicObjType();

    // allocate sonic gateway object id
    sonicGatewayNHGID = m_sonic_id_manager.allocateID(sType);
    if (sonicGatewayNHGID == 0) {
        SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
        return -1;
    }

    // create the SonicGateWayNHGObject and set the id of object
    switch (entry->getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            entry->createSRv6GatewayObjFromRIBEntry(sonicObj);
            sonicObj.id = sonicGatewayNHGID;
            break;
        }
        default: {
            m_sonic_id_manager.freeID(sType, sonicGatewayNHGID);
            SWSS_LOG_ERROR("Unsupported sonic nhg type %d", sType);
            return -1;
        }
    }

    // add the SonicGateWayNHGEntry
    ret = m_sonic_nhg_table->addEntry(sonicObj);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add sonic nhg %d", sonicGatewayNHGID);
        m_sonic_id_manager.freeID(sType, sonicGatewayNHGID);
        return ret;
    }

    SonicGateWayNHGEntry *sonicEntry = m_sonic_nhg_table->getEntry(sType, sonicGatewayNHGID);
    if (sonicEntry == nullptr) {
        SWSS_LOG_ERROR("Failed to get sonic nhg %d", sonicGatewayNHGID);
        m_sonic_id_manager.freeID(sType, sonicGatewayNHGID);
        return ret;
    }

    // write to DB
    ret = m_sonic_nhg_table->writeToDB(sonicEntry);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", sonicGatewayNHGID);
        m_sonic_id_manager.freeID(sType, sonicGatewayNHGID);
        m_sonic_nhg_table->delEntry(sType, sonicGatewayNHGID);
        return ret;
    }

    // set the sonic gateway nhg id of rib entry
    entry->setSonicGatewayObjId(sonicGatewayNHGID);
    SWSS_LOG_NOTICE("Create sonic gateway nhg object for %d, sonic gateway id: %d, type: %d", entry->getRIBID(), sonicGatewayNHGID, entry->getSonicObjType());
    return 0;
}

/*
 * Update the sonic gateway nhg object from RIB entry
 */
int NHGMgr::updateSonicGatewayNHGObject(RIBNHGEntry *entry, uint32_t previousSonicGatewayObjID) {
    SonicGateWayNHGObject sonicObj;
    int ret = 0;
    sonicNhgObjType sType = entry->getSonicObjType();
    if (previousSonicGatewayObjID == 0) {
        SWSS_LOG_ERROR("Invalid previousSonicGatewayObjID");
        return -1;
    }

    // create the SonicGateWayNHGObject and set the id of object
    switch (entry->getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            entry->createSRv6GatewayObjFromRIBEntry(sonicObj);
            sonicObj.id = previousSonicGatewayObjID;
            break;
        }
        default: {
            SWSS_LOG_ERROR("Unsupported sonic nhg type %d", sType);
            return -1;
        }
    }

    ret = m_sonic_nhg_table->updateEntry(sonicObj);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to update sonic nhg %d", previousSonicGatewayObjID);
        return ret;
    }

    SonicGateWayNHGEntry *sonicEntry = m_sonic_nhg_table->getEntry(sType, previousSonicGatewayObjID);
    if (sonicEntry == nullptr) {
        SWSS_LOG_ERROR("Failed to get sonic nhg %d", previousSonicGatewayObjID);
        return ret;
    }

    ret = m_sonic_nhg_table->writeToDB(sonicEntry);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", previousSonicGatewayObjID);
        return ret;
    }
    return 0;
}

/*
 * NHG remove entrance for FIB Block
 */
int NHGMgr::delNHGFull(uint32_t id) {

    if (!m_rib_nhg_table->isNHGExist(id)) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    RIBNHGEntry *entry = m_rib_nhg_table->getEntry(id);
    if (entry == nullptr) {
        return 0;
    }
    uint32_t sonicObjID = entry->getSonicObjID();

    // del the sonic gateway nhg first
    if (entry->hasSonicGatewayObj()) {
        uint32_t sonicGatewayNHGObjID = entry->getSonicGatewayObjID();
        m_sonic_nhg_table->delEntry(entry->getSonicObjType(), sonicGatewayNHGObjID);
        m_sonic_id_manager.freeID(entry->getSonicObjType(), sonicGatewayNHGObjID);
    }

    if (m_rib_nhg_table->delEntry(id) != 0) {
        return -1;
    }
    m_sonic_id_manager.freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicObjID);
    return 0;
}

/*
 * Dump Zebra NHG table
 * not implemented
 */
void NHGMgr::dumpZebraNhgTable(string &ret) {
    m_rib_nhg_table->dump_table(ret);
}

/*
 * get RIB NHG entry by key
 * not implemented
 */
RIBNHGEntry *NHGMgr::getRIBNHGEntryByKey(string key) {
    return NULL;
}

/*
 * get RIB NHG entry by RIB ID
 */
RIBNHGEntry *NHGMgr::getRIBNHGEntryByRIBID(uint32_t id) {
    return m_rib_nhg_table->getEntry(id);
}

/*
 * get Sonic Gateway NHG entry by SonicGateWayNHGObjectKey
 */
SonicGateWayNHGEntry *NHGMgr::getSonicGatewayNHGByKey(SonicGateWayNHGObjectKey key) {
    return m_sonic_nhg_table->getEntry(key);
}

/*
 * get Sonic Gateway NHG entry by Sonic Gateway NHG ID and type
 */
SonicGateWayNHGEntry *NHGMgr::getSonicGatewayNHGByID(sonicNhgObjType type, uint32_t id) {
    return m_sonic_nhg_table->getEntry(type, id);
}

/*
 * check if the sonic NHG id is in used
 */
bool NHGMgr::isSonicNHGIDInUsed(uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, id);
}

/*
 * get Sonic Gateway NHG entry by RIB NHG ID
 */
SonicGateWayNHGEntry *NHGMgr::getSonicGatewayNHGByRIBID(uint32_t id) {
    RIBNHGEntry *ribnhgEntry = getRIBNHGEntryByRIBID(id);
    if (ribnhgEntry == nullptr) {
        return nullptr;
    }
    if (!ribnhgEntry->hasSonicGatewayObj()) {
        return nullptr;
    }
    return getSonicGatewayNHGByID(ribnhgEntry->getSonicObjType(), ribnhgEntry->getSonicGatewayObjID());;
}

/*
 * check if the sonic gateway nhg object id is in used
 */
bool NHGMgr::isSonicGatewayNHGIDInUsed(sonicNhgObjType type, uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(type, id);
}

/*
 * dump NHG group full for debugging
 */
void NHGMgr::dumpNHGGroupFull(fib::NextHopGroupFull nhg) {
    SWSS_LOG_NOTICE("NHG ID %d, type %d, ifname %s", nhg.id, nhg.type, nhg.ifname.c_str());

    if (nhg.type == fib::NEXTHOP_TYPE_IPV4 || nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        SWSS_LOG_NOTICE("   gateway %s", gateway);
    }
    if (nhg.type == fib::NEXTHOP_TYPE_IPV6 || nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        SWSS_LOG_NOTICE("   gateway %s", gateway);
    }
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        SWSS_LOG_NOTICE("   group member %d, num_direct %d", it->id, it->num_direct);
    }
    if (nhg.nh_srv6 != nullptr && nhg.nh_srv6->seg6_segs != nullptr){
        char seg_str[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.nh_srv6->seg6_segs->seg[0], seg_str, INET6_ADDRSTRLEN);
        SWSS_LOG_NOTICE("   srv6 seg6_segs %s", seg_str);
        char seg_src[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.src, seg_src, INET6_ADDRSTRLEN);
        SWSS_LOG_NOTICE("   srv6 seg_src %s", seg_src);
    }
}

/*
 * check if the sonic NHG object id is in used
 */
bool SonicIDMgr::isSonicObjIDUsed(sonicNhgObjType type, uint32_t id) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator != nullptr) {
        return allocator->isInUsed(id);
    }
    return false;
}

RIBNHGTable::RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable)
    : m_nexthop_groupTable(pipeline, tableName, isStateTable) {
}

/*
 * get RIB NHG entry by RIB ID
 */
RIBNHGEntry *RIBNHGTable::getEntry(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it == m_nhg_map.end()) {
        return nullptr;
    }
    return it->second;
}

/*
 * update RIB NHG entry from NHG full
 */
void RIBNHGEntry::checkNeedUpdate(NextHopGroupFull newNhg, uint8_t afNew ,bool &updated, bool &updatedDependency) {

    if (newNhg.weight != m_nhg.weight) {
        updated = true;
    }
    if (newNhg.vrf_id != m_nhg.vrf_id) {
        updated = true;
    }
    if (newNhg.ifindex != m_nhg.ifindex) {
        updated = true;
    }
    if (newNhg.ifname != m_nhg.ifname) {
        updated = true;
    }

    if (afNew != m_af) {
        updated = true;
    }

    if (newNhg.type != m_nhg.type){
        updated = true;
    }

    if (newNhg.type != fib::NEXTHOP_TYPE_IFINDEX && newNhg.type != fib::NEXTHOP_TYPE_BLACKHOLE){
        if(memcmp(&newNhg.gate, &m_nhg.gate, sizeof(fib::g_addr))!=0){
            updated = true;
        }
    }

    if (newNhg.type == fib::NEXTHOP_TYPE_BLACKHOLE){
        if (newNhg.bh_type != m_nhg.bh_type){
            updated = true;
        }
    }

    if (!compareDependsAndDependents(&newNhg, &m_nhg)) {
        updatedDependency = true;
        updated = true;

    }

    if (!compareNHGFullList(&newNhg, &m_nhg)){
        updated = true;
    }


    if (!compareNHGSRv6Fields(&newNhg, &m_nhg)){
        updated = true;
    }

    return;
}

/*
 * get Sonic NHG object id of RIB NHG entry
 */
uint32_t RIBNHGEntry::getSonicObjID() {
    return m_sonic_obj_id;
};

/*
 * get RIB ID of RIB NHG entry
 */
uint32_t RIBNHGEntry::getRIBID() {
    return m_rib_id;
}

/*
 * delete RIB NHG entry by RIB ID
 */
int RIBNHGTable::delEntry(uint32_t id) {
    if (m_nhg_map.find(id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    RIBNHGEntry *entry = m_nhg_map[id];
    if (entry->getDependentsID().size() != 0) {
        SWSS_LOG_ERROR("NextHop group id %d still has dependents.", id);
        return -1;
    }

    removeNHGDependents(entry->getDependsID(), entry->getRIBID());
    this->removeFromDB(entry);
    delete entry;
    m_nhg_map.erase(id);
    return 0;
}

/*
 * add RIB NHG entry from NextHopGroupFull
 */
int RIBNHGTable::addEntry(NextHopGroupFull nhg, uint8_t af) {
    if (m_nhg_map.find(nhg.id) != m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d already exists.", nhg.id);
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

    if (addNHGDependents(entry->getDependsID(), entry->getRIBID()) != 0) {
        return -1;
    }

    m_nhg_map.insert(std::make_pair(nhg.id, entry));
    return 0;
}

/*
 * check if NHG fields updated, if updated then update rib entry
 */
int RIBNHGTable::updateEntry(NextHopGroupFull nhg, uint8_t af, bool &updated) {
    if (m_nhg_map.find(nhg.id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not exists.", nhg.id);
        return -1;
    }

    auto it = m_nhg_map.find(nhg.id);
    RIBNHGEntry *entry = it->second;
    int ret = 0;
    bool updatedDependency = false;
    set<uint32_t> previousDepends = entry->getDependsID();

    // check NHG fields whether updated
    entry->checkNeedUpdate(nhg, af, updated, updatedDependency);

    // update rib entry
    if (updated){
        ret = entry->setEntry(nhg, af);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to set entry for %d", nhg.id);
            return ret;
        }
    }

    // update dependency
    if (updatedDependency) {
        set<uint32_t> addSet, removeSet;
        diffDependency(previousDepends, entry->getDependsID(), addSet, removeSet);
        if (addNHGDependents(addSet, entry->getRIBID()) != 0) {
            return -1;
        }
        removeNHGDependents(removeSet, entry->getRIBID());
    }
    return 0;
}

/*
 * add NHG dependents relationship in dependents set of RIB NHG entry
 */
int RIBNHGTable::addNHGDependents(set<uint32_t> depends, uint32_t id) {
    for (auto it = depends.begin(); it != depends.end(); it++) {
        RIBNHGEntry *e = getEntry(*it);
        if (e == nullptr) {
            return -1;
        }
        e->addDependentsMember(id);
    }
    return 0;
}

/*
 * remove NHG dependents relationship in dependents set of RIB NHG entry
 */
void RIBNHGTable::removeNHGDependents(set<uint32_t> depends, uint32_t id) {
    for (auto it = depends.begin(); it != depends.end(); it++) {
        RIBNHGEntry *e = getEntry(*it);
        if (e == nullptr) {
            continue;
        }
        e->removeDependentsMember(id);
    }
}

/*
 * check if NHG entry exists
 */
bool RIBNHGTable::isNHGExist(uint32_t id) {
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

    if (!entry->isEntryNeedOffload()) {
        SWSS_LOG_INFO("Skip offload of NHG %dr", entry->getNHG().id);
        return 0;
    }

    vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getNHG().id);
        return -1;
    }
    m_nexthop_groupTable.set(std::to_string(entry->getSonicObjID()), fvVector);
    return 0;
}

/*
 * remove Sonic NHG Object from APP_DB
 */
void RIBNHGTable::removeFromDB(RIBNHGEntry *entry) {
    m_nexthop_groupTable.del(std::to_string(entry->getSonicObjID()));
    return;
}

/*
 * dump RIB NHG table
 * not implemented
 */
void RIBNHGTable::dump_table(string &ret) {
    return;
}

/*
 * clean all RIB NHG entry in table
 */
void RIBNHGTable::cleanUp() {
    for (auto it = m_nhg_map.begin(); it != m_nhg_map.end(); it++) {
        delete it->second;
    }
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
 * create the corresponding SonicGateWayNHGObject from RIB NHG Entry
 */
int RIBNHGEntry::createSonicNHGObjFromRIBEntry(SonicGateWayNHGObject &sonicNhgOut) {
    switch (getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY:
            createSRv6GatewayObjFromRIBEntry(sonicNhgOut);
            return 0;
        default:
            SWSS_LOG_ERROR("Unsupported SonicGateWayNHGObject type: %d", getSonicObjType());
            return -1;
    }
}

/*
 * create the corresponding SonicGateWayNHGObject in SRv6 VPN case from RIB NHG Entry
 */
int RIBNHGEntry::createSRv6GatewayObjFromRIBEntry(SonicGateWayNHGObject &sonicNhgOut) {
    sonicNhgOut.groupMember.clear();
    sonicNhgOut.type = SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY;
    sonicNhgOut.id = getSonicGatewayObjID();

    // If this entry has its own SRv6 VPN fields, use them directly.
    // (single-hop or self-contained SRv6 VPN nexthop)
    if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr) {
        sonicNhgOut.nexthop = m_nexthop;
        sonicNhgOut.segSrc = m_segSrc;
        sonicNhgOut.vpnSid = m_vpnSid;
        sonicNhgOut.ifName = m_ifName;
        return 0;
    }

    // Multi-hop: only collect direct depends (m_depends) that have sonic gateway obj as group members.
    // Use weight from m_group map.
    for (auto dep_id: m_depends) {
        RIBNHGEntry *memberEntry = m_table->getEntry(dep_id);
        if (memberEntry == nullptr) {
            SWSS_LOG_ERROR("RIBNHGEntry is not exist: %d", dep_id);
            return -1;
        }
        if (memberEntry->hasSonicGatewayObj() && memberEntry->getSonicObjType() == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
            if (memberEntry->getSonicGatewayObjID() != 0) {
                auto git = m_group.find(dep_id);
                uint8_t weight = (git != m_group.end()) ? git->second : 1;
                sonicNhgOut.groupMember.push_back(std::make_pair(memberEntry->getSonicGatewayObjID(), weight));
            }
        }
    }

    // if no group member, then set the vpn fields
    if (sonicNhgOut.groupMember.size() == 0) {
        sonicNhgOut.nexthop = m_nexthop;
        sonicNhgOut.segSrc = m_segSrc;
        sonicNhgOut.vpnSid = m_vpnSid;
        sonicNhgOut.ifName = m_ifName;
    }
    return 0;
}

/* add the id which depends the entry */
void RIBNHGEntry::addDependentsMember(uint32_t id) {
    m_dependents.insert(id);
}

/* remove the id which not depends the entry anymore */
void RIBNHGEntry::removeDependentsMember(uint32_t id) {
    m_dependents.erase(id);
}

/* getter of group */
unordered_map<uint32_t, uint8_t> RIBNHGEntry::getGroup() {
    return m_group;
}

/* getter of resolved group */
unordered_map<uint32_t, uint8_t> RIBNHGEntry::getResolvedGroup() {
    return m_resolvedGroup;
}

/* getter of depends id set */
set<uint32_t> RIBNHGEntry::getDependsID() {
    return m_depends;
}

/* getter of dependents id set */
set<uint32_t> RIBNHGEntry::getDependentsID() {
    return m_dependents;
}

/* getter of nexthop key */
NexthopMapKey RIBNHGEntry::getKey() {
    return m_key;
}

/* getter of private nexthopgroupfull */
NextHopGroupFull RIBNHGEntry::getNHG() {
    return m_nhg;
}

/* getter of private fvVector */
vector<FieldValueTuple> RIBNHGEntry::getFvVector() {
    return m_fvVector;
}

string RIBNHGEntry::getNextHopStr() {
    return m_nexthop;
}

/* set an RIB Entry from a nexthopgroupfull */
int RIBNHGEntry::setEntry(NextHopGroupFull nhg, uint8_t af) {
    m_key = NexthopMapKey(&nhg);
    m_rib_id = nhg.id;
    m_nhg = nhg;
    m_af = af;
    m_group.clear();
    m_depends.clear();
    m_resolvedGroup.clear();

    // check the depends NHG entry and update m_depends set
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {
        // validate group member
        if (!m_table->isNHGExist(*it)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", *it);
            return -1;
        }
        m_depends.insert(*it);
        SWSS_LOG_NOTICE("NextHop id %d add depends %d.", m_rib_id, *it);
    }


    // check the full list NHG entry and update m_group set
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        // validate group member
        if (!m_table->isNHGExist(it->id)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", it->id);
            return -1;
        }
        m_group.insert(std::make_pair(it->id, it->weight));
        SWSS_LOG_NOTICE("NextHop id %d add group %d.", m_rib_id, it->id);
    }

    /*
     * check if we need to create sonic gateway nhg obj
     * and sonic gateway obj type
     * */
    needCreateSonicGatewayNHGObj();

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

    return 0;
}

/* set the sonic obj id */
void RIBNHGEntry::setSonicNHGObjId(uint32_t id) {
    m_sonic_obj_id = id;
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

    SWSS_LOG_NOTICE("NextHopGroup table set: nexthop[%s] ifname[%s] weight[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                  m_weight.c_str());
    return 0;
}

/* get fields from entry */
int RIBNHGEntry::getNHGFields() {

    if (m_has_member) {
        SWSS_LOG_NOTICE("multi nexthop group");
        // nexthop with group member
        return getNextHopGroupFields();

    } else {
        // nexthop without group member
        SWSS_LOG_NOTICE("single nexthop group");
        return getNextHopFields();
    }
}

/*
 * get FV vector fields and Sonic gateway Objects fields from multi nexthopgroup entry
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
        uint32_t id = nh.first;
        string weight = to_string(nh.second);
        if (!m_table->isNHGExist(id)) {
            SWSS_LOG_ERROR("NextHop group is incomplete: %d", id);
            return -1;
        }
        RIBNHGEntry *entry = this->m_table->getEntry(id);
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
        SWSS_LOG_NOTICE(" entry nexthop: [%s]", entry->getNextHopStr().c_str());
        ifnames += entry->getInterfaceNameStr();
        SWSS_LOG_NOTICE(" entry interface: [%s]", entry->getInterfaceNameStr().c_str());
        weights += weight;
        SWSS_LOG_NOTICE(" entry weight: [%s]", weight.c_str());
        /* SRv6 VPN SID */
        if(m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY){
            if (!vpnSids.empty()){
                vpnSids += NHG_DELIMITER;
            }
            if(!segSrcs.empty()){
                segSrcs += NHG_DELIMITER;
            }
            vpnSids += entry->getVPNSIDStr();
            segSrcs += entry->getSegSrcStr();
            SWSS_LOG_NOTICE(" entry vpnSid: [%s], segSrc: [%s]", vpnSids.c_str(), segSrcs.c_str());
        }

    }
    m_nexthop = nexthops;
    m_ifName = ifnames;
    m_vpnSid = vpnSids;
    m_segSrc = segSrcs;
    m_weight = weights;
    SWSS_LOG_NOTICE("get NextHopGroup fields done, nexthop[%s] ifname[%s] weight[%s] vpnSid[%s] segSrc[%s]", m_nexthop.c_str(), m_ifName.c_str(),
                  m_weight.c_str(), m_vpnSid.c_str(), m_segSrc.c_str());
    return 0;
}

/*
 * get FV vector fields and Sonic gateway Objects fields from multi nexthopgroup entry
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

    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        char sid[INET6_ADDRSTRLEN] = {0};
        char seg_src[INET6_ADDRSTRLEN] = {0};
        if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr){
            inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_segs->seg[0], sid, INET6_ADDRSTRLEN);
            m_vpnSid = sid;
            inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_src, seg_src, INET6_ADDRSTRLEN);
            m_segSrc = seg_src;
        }else{
            SWSS_LOG_ERROR("single nexthop id %d type srv6 gateway has no seg6_segs", m_rib_id);
            return -1;
        }

    }
    return 0;
}

/*
 * get resolved group from nexthopgroupfull
 * below fields of entry will be set:
 *      m_resolvedGroup
 *      m_is_single
 *      m_has_member
 */
int RIBNHGEntry::getResolvedGroupFromNHGFull() {
    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_NORMAL) {
        for (auto nhg: m_nhg.nh_grp_full_list) {
            if (nhg.num_direct == 0) {
                m_resolvedGroup.insert(std::make_pair(nhg.id, nhg.weight));
                SWSS_LOG_NOTICE("NextHop id %d add resolved group %d.", m_rib_id, nhg.id);
            }
        }
    } else if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        for (auto nhg: m_nhg.nh_grp_full_list){
            RIBNHGEntry *entry = m_table->getEntry(nhg.id);
            if (entry == nullptr){
                SWSS_LOG_ERROR("NextHop id %d in group not found.", nhg.id);
                return -1;
            }
            if (entry->hasSonicGatewayObj()){
                m_resolvedGroup.insert(std::make_pair(nhg.id, nhg.weight));
                SWSS_LOG_NOTICE("NextHop id %d add resolved group %d, type %d.", m_rib_id, nhg.id, entry->getSonicObjType());
            }
        }
    }

    if(m_resolvedGroup.size() == 0){
        m_is_single = true;
        m_has_member = false;
    } else if (m_resolvedGroup.size() == 1){
        m_is_single = true;
        m_has_member = true;
    } else{
        m_is_single = false;
        m_has_member = true;
    }
    return 0;
}

/*
 * check if we need to create sonic gateway obj and the object type
 * below fields of entry will be set:
 *      m_sonic_obj_type
 *      m_has_sonic_gateway_obj
 */
void RIBNHGEntry::needCreateSonicGatewayNHGObj() {

    /* now only for srv6 vpn gateway NHG obj */

    /* for srv6 gateway, check if we need to create pic group obj */
    for (auto it = m_group.begin(); it != m_group.end(); it++) {
        RIBNHGEntry *entry = m_table->getEntry(it->first);
        if (entry->hasSonicGatewayObj()) {
            m_sonic_obj_type = entry->getSonicObjType();
            m_has_sonic_gateway_obj = true;
            SWSS_LOG_NOTICE("NextHop id %d has sonic gateway obj, is multi nexthop group.", it->first);
            return;
        }
    }

    /*
     * For srv6 gateway, any nexthop with nh_srv6 and seg6_segs should create a sonic gateway obj.
     * This covers both single-hop SRv6 VPN nexthops (no group members) and
     * recursive SRv6 VPN nexthops that carry their own SRv6 VPN info.
     */
    if (m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr && CHECK_FLAG(m_nhg.nhg_flags, NEXTHOP_GROUP_RECEIVED_FLAG)) {
        m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY;
        m_has_sonic_gateway_obj = true;
        SWSS_LOG_NOTICE("NextHop id %d has sonic gateway obj.", m_rib_id);
        return;
    }

    return;
}

/*
 * check if the entry has sonic gateway obj
 */
bool RIBNHGEntry::hasSonicGatewayObj() {
    return m_has_sonic_gateway_obj;
}

/*
 * get sonic gateway obj type
 */
sonicNhgObjType RIBNHGEntry::getSonicObjType() {
    return m_sonic_obj_type;
}

/*
 * get vpn sid str
 */
string RIBNHGEntry::getVPNSIDStr() {
    return m_vpnSid;
}

/*
 * get sonic gateway obj id
 */
uint32_t RIBNHGEntry::getSonicGatewayObjID() {
    return m_sonic_gateway_nhg_id;
}

/*
 * set sonic gateway obj id
 */
void RIBNHGEntry::setSonicGatewayObjId(uint32_t id) {
    m_sonic_gateway_nhg_id = id;
}

/*
 * get interface name str
 */
string RIBNHGEntry::getInterfaceNameStr() {
    return m_ifName;
}
/*
 * get address family
 */
uint8_t RIBNHGEntry::getAddressFamily() {
    return m_af;
}

/*
 * get seg src str
 */
string RIBNHGEntry::getSegSrcStr() {
    return m_segSrc;
}

void RIBNHGEntry::enableNHG() {
    m_enable = true;
    return ;
}

void RIBNHGEntry::disableNHG() {
    m_enable = false;
    return ;
}

bool RIBNHGEntry::getNhgEnableStatus() {
    return m_enable;
}

SonicNHGTable::SonicNHGTable(RedisPipeline *pipeline, const std::string &picTableName, bool isStateTable) : m_pic_contextTable(pipeline, picTableName, isStateTable) {
}

/*
 * write the Sonic gateway object into DB
 * SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY into PIC_CONTEXT_TABLE
 */
int SonicNHGTable::writeToDB(SonicGateWayNHGEntry *entry) {
    vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getNHG().id);
        return -1;
    }
    if (entry->getType() == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        m_pic_contextTable.set(std::to_string(entry->getSonicGateWayObjID()), fvVector);
    }
    return 0;
}

/*
 * remove the Sonic gateway object from DB
 */
void SonicNHGTable::removeFromDB(SonicGateWayNHGEntry *entry) {
    if (entry->getType() == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        m_pic_contextTable.del(std::to_string(entry->getSonicGateWayObjID()));
    }
    return;
}

/*
 * add the SonicGateWayNHGEntry
 */
int SonicNHGTable::addEntry(SonicGateWayNHGObject sonicObj) {
    SonicGateWayNHGObjectKey key = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(sonicObj);
    if (m_sonic_nhg_map.find(key) != m_sonic_nhg_map.end()) {
        SWSS_LOG_ERROR("SonicGateWayNHGObject is already exist: %d", sonicObj.id);
        return -1;
    }

    SonicGateWayNHGEntry *entry = SonicGateWayNHGEntry::createSonicGateWayNHGEntry(this);
    if (entry == nullptr) {
        return -1;
    }

    // set SonicGateWayNHGEntry fields
    int ret = entry->setEntry(sonicObj);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    m_sonic_nhg_map[key] = entry;
    switch (entry->getType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            m_pic_map[entry->getSonicGateWayObjID()] = entry;
            break;
        }
        default: {
            SWSS_LOG_WARN("Skip SonicGateWayNHGObject type");
            break;
        }
    }

    return 0;
}

/*
 * update the SonicGateWayNHGEntry from SonicGateWayNHGObject
 */
int SonicNHGTable::updateEntry(SonicGateWayNHGObject sonicObj) {
    SonicGateWayNHGObjectKey key = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(sonicObj);
    SonicGateWayNHGEntry *entry = getEntry(sonicObj.type, sonicObj.id);
    if (entry == nullptr || entry->getType() != sonicObj.type || m_sonic_nhg_map.find(key) != m_sonic_nhg_map.end()) {
        SWSS_LOG_WARN("SonicGateWayNHGObject update error");
        return -1;
    }
    SonicGateWayNHGObjectKey previousKey = entry->getSonicGateWayObjKey();

    int ret = entry->setEntry(sonicObj);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    // remove previous key entry pair in m_sonic_nhg_map
    m_sonic_nhg_map[key] = entry;
    m_sonic_nhg_map.erase(previousKey);

    /*
    switch (entry->getType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            m_pic_map[entry->getSonicGateWayObjID()] = entry;
            break;
        }
        default: {
            SWSS_LOG_WARN("Skip SonicGateWayNHGObject type");
            break;
        }
    }*/
    return 0;
}

/*
 * delete the SonicGateWayNHGEntry
 */
void SonicNHGTable::delEntry(SonicGateWayNHGObject sonicObj) {
    SonicGateWayNHGObjectKey key = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(sonicObj);
    if (m_sonic_nhg_map.find(key) == m_sonic_nhg_map.end()) {
        SWSS_LOG_WARN("SonicGateWayNHGObject is not exist");
        return;
    }
    SonicGateWayNHGEntry *entry = m_sonic_nhg_map[key];
    removeFromDB(entry);

    switch (sonicObj.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY:
            m_pic_map.erase(entry->getSonicGateWayObjID());
            break;
        default:
            break;
    }
    m_sonic_nhg_map.erase(entry->getSonicGateWayObjKey());
    delete entry;
    return;
}

/*
 * delete the SonicGateWayNHGEntry by type and id
 */
void SonicNHGTable::delEntry(sonicNhgObjType type, uint32_t id) {
    SonicGateWayNHGEntry *entry = nullptr;
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            if (m_pic_map.find(id) != m_pic_map.end()) {
                entry = m_pic_map[id];
                m_pic_map.erase(id);
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
        SWSS_LOG_WARN("SonicGateWayNHGObject is not exist: %d %d", type, id);
        return;
    }

    removeFromDB(entry);
    m_sonic_nhg_map.erase(entry->getSonicGateWayObjKey());
    delete entry;
    return;
}

/*
 * delete all the SonicGateWayNHGEntry
 */
void SonicNHGTable::cleanUp() {
    for (auto it = m_sonic_nhg_map.begin(); it != m_sonic_nhg_map.end(); it++) {
        SonicGateWayNHGEntry *entry = m_sonic_nhg_map[it->first];
        delete entry;
    }
}

/*
 * get the SonicGateWayNHGEntry by SonicGateWayNHGObjectKey
 */
SonicGateWayNHGEntry *SonicNHGTable::getEntry(SonicGateWayNHGObjectKey key) {
    if (m_sonic_nhg_map.find(key) != m_sonic_nhg_map.end()) {
        return m_sonic_nhg_map[key];
    }
    return nullptr;
}

/*
 * get the SonicGateWayNHGEntry by type and id
 */
SonicGateWayNHGEntry *SonicNHGTable::getEntry(sonicNhgObjType type, uint32_t sonicID) {
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            if (m_pic_map.find(sonicID) != m_pic_map.end()) {
                return m_pic_map[sonicID];
            }
            return nullptr;
        }
        default: {
            SWSS_LOG_ERROR("Failed to get SonicGateWayNHGEntry: %d %d", type, sonicID);
            return nullptr;
        }
    }
}

/*
 * create SonicGateWayNHGEntry
 */
SonicGateWayNHGEntry *SonicGateWayNHGEntry::createSonicGateWayNHGEntry(SonicNHGTable *mTable) {
    SonicGateWayNHGEntry *entry = new SonicGateWayNHGEntry(mTable);
    return entry;
}

/*
 * return the fvVector
 */
vector<FieldValueTuple> SonicGateWayNHGEntry::getFvVector() {
    return m_fvVector;
}

/*
 * sync the fvVector for SonicGateWayNHGEntry
 */
int SonicGateWayNHGEntry::syncFvVector() {
    m_fvVector.clear();
    switch (m_sonic_obj.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            return syncFvVectorForSRv6Gateway();
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicGateWayNHGObject type: %d", m_sonic_obj.type);
            return -1;
        }
    }
}

/*
 * sync the fvVector for SRv6 Gateway
 */
int SonicGateWayNHGEntry::syncFvVectorForSRv6Gateway() {
    if (m_sonic_obj.groupMember.size() == 0) {
        m_fvVector.push_back(FieldValueTuple("nexthop", m_sonic_obj.nexthop));
        m_fvVector.push_back(FieldValueTuple("ifname", m_sonic_obj.ifName));
        m_fvVector.push_back(FieldValueTuple("seg_src", m_sonic_obj.segSrc));
        m_fvVector.push_back(FieldValueTuple("vpn_sid", m_sonic_obj.vpnSid));
    }else {
        string nexthops = "";
        string vpnSids = "";
        string ifnames = "";
        string weights = "";
        string segSrcs = "";
        for (auto member: m_group) {
            SonicGateWayNHGEntry *entry = m_table->getEntry(SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, member.first);
            if (entry == nullptr) {
                SWSS_LOG_ERROR("SonicGateWayNHGObject is not exist: %d", member.first);
                return -1;
            }

            SonicGateWayNHGObject nhg = entry->getNHG();
            if (nhg.groupMember.size() > 0) {
                SWSS_LOG_ERROR("SonicGateWayNHGObject member is a group: %d", member.first);
                return -1;
            }

            if (!nexthops.empty()) {
                nexthops += NHG_DELIMITER;
            }
            nexthops += nhg.nexthop;

            if (!ifnames.empty()) {
                ifnames += NHG_DELIMITER;
            }
            if (!nhg.ifName.empty()) {
                ifnames += nhg.ifName;
            } else {
                ifnames += "unknown";
            }

            if (!vpnSids.empty()) {
                vpnSids += NHG_DELIMITER;
            }
            vpnSids += nhg.vpnSid;

            if (!weights.empty()) {
                weights += NHG_DELIMITER;
            }
            weights += std::to_string(member.second);

            if (!segSrcs.empty()) {
                segSrcs += NHG_DELIMITER;
            }
            segSrcs += nhg.segSrc;
        }
        m_fvVector.push_back(FieldValueTuple("nexthop", nexthops));
        m_fvVector.push_back(FieldValueTuple("ifname", ifnames));
        m_fvVector.push_back(FieldValueTuple("weight", weights));
        m_fvVector.push_back(FieldValueTuple("vpn_sid", vpnSids));
        m_fvVector.push_back(FieldValueTuple("seg_src", segSrcs));
    }
    return 0;
}

int SonicGateWayNHGEntry::setEntry(SonicGateWayNHGObject nhg) {
    switch (nhg.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            setSRv6GatewayEntry(nhg);
            break;
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicGateWayNHGObject type: %d", nhg.type);
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

int SonicGateWayNHGEntry::setSRv6GatewayEntry(SonicGateWayNHGObject nhg) {
    m_sonic_obj = nhg;
    m_sonic_obj_id = nhg.id;
    m_sonic_obj_key = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(nhg);
    for (auto member: nhg.groupMember) {
        SonicGateWayNHGEntry *entry = m_table->getEntry(nhg.type, member.first);
        if (entry == nullptr) {
            SWSS_LOG_ERROR("SonicGateWayNHGObject is not exist");
            return -1;
        }
        m_group.insert(std::make_pair(entry->getSonicGateWayObjID(), member.second));
    }
    return 0;
}

SonicGateWayNHGObject SonicGateWayNHGEntry::getNHG() {
    return m_sonic_obj;
}

SonicGateWayNHGEntry::SonicGateWayNHGEntry(SonicNHGTable *mTable) : m_table(mTable) {
}

SonicGateWayNHGEntry::~SonicGateWayNHGEntry() {
    m_table = nullptr;
}

u_int32_t SonicIDMgr::allocateID(sonicNhgObjType type) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator == nullptr) {
        SWSS_LOG_ERROR("SonicIDAllocator is not exist: %d", type);
        return 0;
    }
    return allocator->allocateID();
}

void SonicIDMgr::freeID(sonicNhgObjType type, uint32_t id) {
    SonicIDAllocator *allocator = getAllocator(type);
    if (allocator == nullptr) {
        SWSS_LOG_ERROR("SonicIDAllocator is not exist: %d", type);
        return;
    }
    allocator->freeID(id);
}

SonicIDAllocator *SonicIDMgr::getAllocator(sonicNhgObjType type) {
    switch (type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY:
            return m_pic_id_allocator;
        case SONIC_NHG_OBJ_TYPE_NHG_NORMAL:
            return m_nhg_id_allocator;
        default:
            return nullptr;
    }
}

int SonicIDMgr::init(vector<sonicNhgObjType> supportedObjs) {
    for (auto type: supportedObjs) {
        switch (type) {
            case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
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
                SWSS_LOG_ERROR("Unsupported SonicGateWayNHGObject type: %d", type);
                return -1;
            }
        }
    }
    return 0;
}

uint32_t SonicIDAllocator::allocateID() {
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

void SonicIDAllocator::freeID(uint32_t id) {
    if (m_id_map.find(id) != m_id_map.end()) {
        m_id_map.erase(id);
    }
}

bool SonicIDAllocator::isInUsed(uint32_t id) {
    if (m_id_map.find(id) != m_id_map.end()) {
        return true;
    }
    return false;
}

/*
 * recover SonicIDAllocator from DB
 * not implemented
 */
int SonicIDAllocator::recoverSonicIDMapFromDB() {
    return 0;
}

/*
 * create SonicGateWayNHGObjectKey from SonicGateWayNHGObject
 */
SonicGateWayNHGObjectKey SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(SonicGateWayNHGObject obj) {
    SonicGateWayNHGObjectKey key;
    key.groupMember = obj.groupMember;
    key.type = obj.type;
    if (obj.type == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        key.nexthop = obj.nexthop;
        key.segSrc = obj.segSrc;
        key.vpnSid = obj.vpnSid;
    }
    return key;
}

/*
 * create SonicGateWayNHGObjectKey from RIBNHGEntry
 */
int SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(RIBNHGEntry *entry, SonicGateWayNHGObjectKey &key_out) {
    SonicGateWayNHGObject obj;
    int ret = entry->createSonicNHGObjFromRIBEntry(obj);
    if (ret != 0) {
        return ret;
    }
    key_out = createSonicGateWayNHGObjectKey(obj);
    return 0;
}



/*
 * Not implemented
 */
NexthopMapKey::NexthopMapKey(const NextHopGroupFull *nhg) {
    //int key = nhg.id;
    /*for (int i = 0; i < nhg->depends.size(); i++)
	{
		if (i == 0)
		{
			key = key + "group:";
		}
		key = key + "ribID" + nhg->depends[i].ribID + "weight" + nhg->depends[i].weight;
	}
	switch (nhg->type)
	{
	case NEXTHOP_TYPE_IFINDEX:
	{
		key = key + "type:" + nhg->type;
		key = key + "ifindex:" + nhg->ifindex;
		key = key + "vrf_id" + nhg->vrf_id;
		break ;
	}
	case NEXTHOP_TYPE_IPV4:
	{
		key = key + "type:" + nhg->type;
		key = key + "ifindex:" + nhg->ifindex;
		key = key + "vrf_id" + nhg->vrf_id;
		key = key + "gate" + nhg->gate.ipv4;
		break ;

}
case NEXTHOP_TYPE_IPV4_IFINDEX:
{
    key = key + "type:" + nhg->type;
    key = key + "ifindex:" + nhg->ifindex;
    key = key + "vrf_id" + nhg->vrf_id;
    key = key + "gate" + nhg->gate.ipv4;
    break ;

}
case NEXTHOP_TYPE_IPV6:
{
    key = key + "type:" + nhg->type;
    key = key + "ifindex:" + nhg->ifindex;
    key = key + "vrf_id" + nhg->vrf_id;
    key = key + "gate" + nhg->gate.ipv6;
    break ;

}
case NEXTHOP_TYPE_IPV6_IFINDEX:
{
    key = key + "type:" + nhg->type;
    key = key + "ifindex:" + nhg->ifindex;
    key = key + "vrf_id" + nhg->vrf_id;
    key = key + "gate" + nhg->gate.ipv6;
    break ;

}
case NEXTHOP_TYPE_BLACKHOLE:
{
    key = key + "type:" + nhg->type;
    //key = key + "blackhole_type:" + nhg->type;
    break ;
}
default:
    break ;
    }
    m_address_key = key;
    */
}

NexthopMapKey::NexthopMapKey() {
    }