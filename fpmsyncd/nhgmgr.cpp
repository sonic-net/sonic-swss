#include "nhgmgr.h"
#include "logger.h"
#include <string.h>


using namespace std;
using namespace swss;

NHGMgr::NHGMgr(RedisPipeline *pipeline, const std::string &nexthopTableName, const std::string &picTableName, bool isStateTable) {
    m_rib_nhg_table = new RIBNHGTable(pipeline, nexthopTableName, isStateTable);
    m_sonic_nhg_table = new SonicNHGTable(pipeline, picTableName, isStateTable);
    m_sonic_id_manager.init({SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY, SONIC_NHG_OBJ_TYPE_NHG_NORMAL});
}

int NHGMgr::addNHGFull(NextHopGroupFull nhg) {
    int ret = 0;
    SWSS_LOG_INFO("Receiving NHG %d, type %d", nhg.id, nhg.type);
    dumpNHGGroupFull(nhg);
    // insert rib nhg in rib table
    RIBNHGEntry *entry;
    bool updated = false;
    if (m_rib_nhg_table->isNHGExist(nhg.id)) {
        entry = m_rib_nhg_table->getEntry(nhg.id);
        if (entry == nullptr) {
            SWSS_LOG_ERROR("Failed to get NHG %d", nhg.id);
            return ret;
        }
        bool hasPreviousSonicObj = entry->hasSonicObj();
        uint32_t previousSonicGatewayObjID = entry->getSonicGatewayObjID();
        sonicNhgObjType previousType = entry->getSonicObjType();
        ret = m_rib_nhg_table->updateEntry(nhg, updated);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to update NHG %d", nhg.id);
            return ret;
        }
        entry = m_rib_nhg_table->getEntry(nhg.id);
        if (entry == nullptr) {
            return ret;
        }
        if (updated) {
            ret = m_rib_nhg_table->writeToDB(entry);
            if (ret != 0) {
                SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
                return ret;
            }
            if (entry->hasSonicObj()) {
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
                ret = createSonicNhgObject(entry);
                if (ret != 0) {
                    m_rib_nhg_table->removeFromDB(entry);
                    m_rib_nhg_table->delEntry(nhg.id);
                    SWSS_LOG_ERROR("Failed to create sonic nhg %d", nhg.id);
                    return ret;
                }
                if (hasPreviousSonicObj) {
                    m_sonic_nhg_table->delEntry(previousType, previousSonicGatewayObjID);
                    m_sonic_id_manager.freeID(previousType, previousSonicGatewayObjID);
                }

            } else {
                if (hasPreviousSonicObj) {
                    m_sonic_nhg_table->delEntry(previousType, previousSonicGatewayObjID);
                    m_sonic_id_manager.freeID(previousType, previousSonicGatewayObjID);
                    entry->setSonicGatewayObjId(0);
                }
            }
        }
    } else {
        ret = m_rib_nhg_table->addEntry(nhg);
        if (ret != 0) {
            SWSS_LOG_ERROR("Failed to add NHG %d", nhg.id);
            return ret;
        }
        entry = m_rib_nhg_table->getEntry(nhg.id);
        if (entry == nullptr) {
            return ret;
        }

        if (entry->isEntryNeedOffload()) {
            uint32_t sonicId = m_sonic_id_manager.allocateID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL);
            if (sonicId == 0) {
                SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
                m_rib_nhg_table->delEntry(nhg.id);
                return -1;
            }

            entry->setSonicObjId(sonicId);
            ret = m_rib_nhg_table->writeToDB(entry);
            if (ret != 0) {
                m_rib_nhg_table->delEntry(nhg.id);
                m_sonic_id_manager.freeID(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, sonicId);
                SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
                return ret;
            }
        }

        if (entry->hasSonicObj()) {
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

            ret = createSonicNhgObject(entry);
            if (ret != 0) {
                m_rib_nhg_table->removeFromDB(entry);
                m_rib_nhg_table->delEntry(nhg.id);
                SWSS_LOG_ERROR("Failed to create sonic nhg %d", nhg.id);
                return ret;
            }
        }
    }
    return ret;
}

int NHGMgr::createSonicNhgObject(RIBNHGEntry *entry) {
    // srv6 sonic nhg create
    SonicGateWayNHGObject sonicObj;
    uint32_t sonicGatewayNHGID;
    int ret = 0;
    sonicNhgObjType sType = entry->getSonicObjType();

    sonicGatewayNHGID = m_sonic_id_manager.allocateID(sType);
    if (sonicGatewayNHGID == 0) {
        SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
        return -1;
    }

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
    ret = m_sonic_nhg_table->writeToDB(sonicEntry);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", sonicGatewayNHGID);
        m_sonic_id_manager.freeID(sType, sonicGatewayNHGID);
        m_sonic_nhg_table->delEntry(sType, sonicGatewayNHGID);
        return ret;
    }
    entry->setSonicGatewayObjId(sonicGatewayNHGID);
    return 0;
}

int NHGMgr::updateSonicNhgObject(RIBNHGEntry *entry, uint32_t previousSonicGatewayObjID) {
    SonicGateWayNHGObject sonicObj;
    uint32_t sonicGatewayNHGID;
    int ret = 0;
    sonicNhgObjType sType = entry->getSonicObjType();
    if (previousSonicGatewayObjID == 0) {

        SWSS_LOG_ERROR("Failed to allocate sonic nhg id");
        return -1;
    }
    sonicGatewayNHGID = previousSonicGatewayObjID;

    switch (entry->getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            entry->createSRv6GatewayObjFromRIBEntry(sonicObj);
            sonicObj.id = sonicGatewayNHGID;
            break;
        }
        default: {
            SWSS_LOG_ERROR("Unsupported sonic nhg type %d", sType);
            return -1;
        }
    }

    ret = m_sonic_nhg_table->updateEntry(sonicObj);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to add sonic nhg %d", sonicGatewayNHGID);
        return ret;
    }

    SonicGateWayNHGEntry *sonicEntry = m_sonic_nhg_table->getEntry(sType, sonicGatewayNHGID);
    if (sonicEntry == nullptr) {
        SWSS_LOG_ERROR("Failed to get sonic nhg %d", sonicGatewayNHGID);
        return ret;
    }
    ret = m_sonic_nhg_table->writeToDB(sonicEntry);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", sonicGatewayNHGID);
        return ret;
    }
    entry->setSonicGatewayObjId(sonicGatewayNHGID);
    return 0;
}

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
    if (entry->hasSonicObj()) {
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

void NHGMgr::dumpZebraNhgTable(string &ret) {
    m_rib_nhg_table->dump_table(ret);
}

RIBNHGEntry *NHGMgr::getRIBNHGEntryByKey(string key) {
    return NULL;
}

RIBNHGEntry *NHGMgr::getRIBNHGEntryByRIBID(uint32_t id) {
    return m_rib_nhg_table->getEntry(id);
}

SonicGateWayNHGEntry *NHGMgr::getSonicNHGByKey(SonicGateWayNHGObjectKey key) {
    return m_sonic_nhg_table->getEntry(key);
}


SonicGateWayNHGEntry *NHGMgr::getSonicNHGByID(sonicNhgObjType type, uint32_t id) {
    return m_sonic_nhg_table->getEntry(type, id);
}

bool NHGMgr::isSonicNHGIDInUsed(uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(SONIC_NHG_OBJ_TYPE_NHG_NORMAL, id);
}

SonicGateWayNHGEntry *NHGMgr::getSonicNHGByRIBID(uint32_t id) {
    SonicGateWayNHGEntry *entry = nullptr;
    RIBNHGEntry *ribnhgEntry = getRIBNHGEntryByRIBID(id);
    if (ribnhgEntry == nullptr) {
        return nullptr;
    }
    if (!ribnhgEntry->hasSonicObj()) {
        return nullptr;
    }
    entry = getSonicNHGByID(ribnhgEntry->getSonicObjType(), ribnhgEntry->getSonicGatewayObjID());
    return entry;
}

bool NHGMgr::isSonicGatewayNHGIDInUsed(sonicNhgObjType type, uint32_t id) {
    return m_sonic_id_manager.isSonicObjIDUsed(type, id);
}

void NHGMgr::dumpNHGGroupFull(fib::NextHopGroupFull nhg) {
    SWSS_LOG_INFO("NHG ID %d, type %d, ifname %s", nhg.id, nhg.type, nhg.ifname.c_str());

    if (nhg.type == fib::NEXTHOP_TYPE_IPV4 || nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        SWSS_LOG_INFO("   NHG gateway %s", gateway);
    }
    if (nhg.type == fib::NEXTHOP_TYPE_IPV6 || nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        SWSS_LOG_INFO("   NHG gateway %s", gateway);
    }
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        SWSS_LOG_INFO("   NHG ID %d, num_direct %d", it->id, it->num_direct);
    }
}

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

RIBNHGEntry *RIBNHGTable::getEntry(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it == m_nhg_map.end()) {
        return nullptr;
    }
    return it->second;
}

int RIBNHGEntry::updateEntryFromNHGFull(NextHopGroupFull newNhg, bool &updated, bool &updatedDependency) {

    if (newNhg.weight != m_nhg.weight) {
        m_nhg.weight = newNhg.weight;
        updated = true;
    }
    if (newNhg.vrf_id != m_nhg.vrf_id) {
        m_nhg.vrf_id = newNhg.vrf_id;
        updated = true;
    }
    if (newNhg.ifindex != m_nhg.ifindex) {
        m_nhg.ifindex = newNhg.ifindex;
        updated = true;
    }
    if (newNhg.ifname != m_nhg.ifname) {
        m_nhg.ifname = newNhg.ifname;
        updated = true;
    }
    if (!compareDependsAndDependents(&newNhg, &m_nhg)) {
        m_nhg.depends = newNhg.depends;
        m_nhg.dependents = newNhg.dependents;
        updatedDependency = true;
    }
    string nexthopsOld, nexthopsNew = "";
    string ifnamesOld, ifnamesNew = "";
    string weightsOld, weightsNew = "";
    uint8_t afOld, afNew = -1;

    if (getNHGFields(m_nhg, nexthopsOld, ifnamesOld, weightsOld, afOld) != 0) {
        return -1;
    }

    if (getNHGFields(newNhg, nexthopsNew, ifnamesNew, weightsNew, afNew) != 0) {
        return -1;
    }
    if (nexthopsOld != nexthopsNew) {
        updated = true;
    }
    if (ifnamesNew != ifnamesOld) {
        updated = true;
    }
    if (weightsNew != weightsOld) {
        updated = true;
    }
    if (afNew != afOld) {
        updated = true;
    }
    if (updated) {
        int ret = this->setEntry(newNhg);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

bool RIBNHGEntry::compareDependsAndDependents(const NextHopGroupFull *new_nhg, const NextHopGroupFull *oldNHG) {
    if ((new_nhg->depends.size()) != (oldNHG->depends.size())) {
        return false;
    }
    if ((new_nhg->dependents.size()) != (oldNHG->dependents.size())) {
        return false;
    }
    set<int> new_nhg_depends;
    set<int> old_nhg_depends;
    set<int> new_nhg_dependents;
    set<int> old_nhg_dependents;
    for (auto it = new_nhg->depends.begin(); it != new_nhg->depends.end(); it++) {
        new_nhg_depends.insert(*it);
    }
    for (auto it = new_nhg->dependents.begin(); it != new_nhg->dependents.end(); it++) {
        new_nhg_dependents.insert(*it);
    }
    for (auto it = oldNHG->depends.begin(); it != oldNHG->depends.end(); it++) {
        old_nhg_depends.insert(*it);
        if (new_nhg_depends
                    .find(*it) == new_nhg_depends.end()) {
            return false;
        }
    }
    for (auto it = oldNHG->dependents.begin(); it != oldNHG->dependents.end(); it++) {
        old_nhg_dependents.insert(*it);
        if (new_nhg_dependents
                    .find(*it) == new_nhg_dependents.end()) {
            return false;
        }
    }
    for (auto it = new_nhg->depends.begin(); it != new_nhg->depends.end(); it++) {
        if (old_nhg_depends
                    .find(*it) == old_nhg_depends.end()) {
            return false;
        }
    }
    for (auto it = new_nhg->dependents.begin(); it != new_nhg->dependents.end(); it++) {
        if (old_nhg_dependents
                    .find(*it) == old_nhg_dependents.end()) {
            return false;
        }
    }
    return true;
}

uint32_t RIBNHGEntry::getSonicObjID() {
    return m_sonic_obj_id;
};

uint32_t RIBNHGEntry::getRIBID() {
    return m_rib_id;
}

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

int RIBNHGTable::addEntry(NextHopGroupFull nhg) {
    if (m_nhg_map.find(nhg.id) != m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d already exists.", nhg.id);
        return -1;
    }

    RIBNHGEntry *entry = RIBNHGEntry::createNHGEntry(this);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }
    int ret = entry->setEntry(nhg);
    if (ret != 0) {
        delete entry;
        return -1;
    }

    if (addNHGDependents(entry->getDependsID(), entry->getRIBID()) != 0) {
        return -1;
    }

    m_nhg_map.insert(std::make_pair(nhg.id, entry));
    //m_key_2_id_map.insert(std::make_pair(entry->getKey(), entry));
    return 0;
}

int RIBNHGTable::updateEntry(NextHopGroupFull nhg, bool &updated) {
    if (m_nhg_map.find(nhg.id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not exists.", nhg.id);
        return -1;
    }

    auto it = m_nhg_map.find(nhg.id);
    RIBNHGEntry *entry = it->second;

    bool updatedDependency = false;
    set<uint32_t> previous_depends = entry->getDependsID();
    int ret = entry->updateEntryFromNHGFull(nhg, updated, updatedDependency);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }
    set<uint32_t> current_depends = entry->getDependsID();

    if (updatedDependency) {
        set<uint32_t> addSet, removeSet;
        diffDependency(previous_depends, current_depends, addSet, removeSet);
        if (addNHGDependents(addSet, entry->getRIBID()) != 0) {
            return -1;
        }
        removeNHGDependents(removeSet, entry->getRIBID());
    }
    return 0;
}

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
bool RIBNHGTable::isNHGExist(string key)
{
    auto it = m_key_2_id_map.find(key);
    if (it != m_key_2_id_map.end())
    {
        return true;
    }
    return false;
}
*/
bool RIBNHGTable::isNHGExist(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it != m_nhg_map.end()) {
        return true;
    }
    return false;
}

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

void RIBNHGTable::removeFromDB(RIBNHGEntry *entry) {
    m_nexthop_groupTable.del(std::to_string(entry->getSonicObjID()));
    return;
}

void RIBNHGTable::dump_table(string &ret) {
    string indent = "    ";
    for (auto it = m_nhg_map.begin(); it != m_nhg_map.end(); it++) {
        NextHopGroupFull nhg = it->second->getNHG();
        ret += "nhg id: " + to_string(nhg.id) + ":\n";
        /*if (it->second->getGroup().size() != 0) {
			ret += indent + "group: ";
			for (auto it2 = it->second->getGroup().begin(); it2 != it->second->getGroup().end(); it2++) {
				ret += to_string(it2->first) + ":" + to_string(it2->second) + ",";
			}
		}*/

        /*if (!it->second->getNexthop().empty()) {
			ret += indent + "nexthop: " + it->second->getNexthop() + "\n";
		}*/

        switch (nhg.type) {
            case fib::NEXTHOP_TYPE_IFINDEX:
                ret += indent + "type: " + "NEXTHOP_TYPE_IFINDEX" + "\n";
                ret += indent + "interface: " + to_string(nhg.ifindex) + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV4:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV4" + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV4_IFINDEX:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV4_IFINDEX" + "\n";
                ret += indent + "interface: " + to_string(nhg.ifindex) + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV6_IFINDEX:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV6_IFINDEX" + "\n";
                ret += indent + "interface: " + to_string(nhg.ifindex) + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV6:
                ret += indent + "type: " + "NEXTHOP_TYPE_IPV6" + "\n";
                break;
            default:
                SWSS_LOG_ERROR("Unknown NextHop type %d", nhg.type);
                return;
        }
    }
    return;
}

void RIBNHGTable::diffDependency(set<uint32_t> oldSet, set<uint32_t> newSet, set<uint32_t> &addSet,
                                 set<uint32_t> &removeSet) {
    for (auto it = oldSet.begin(); it != oldSet.end(); it++) {
        if (newSet.find(*it) == newSet.end()) {
            removeSet.insert(*it);
        }
    }
    for (auto it = newSet.begin(); it != newSet.end(); it++) {
        if (oldSet.find(*it) == oldSet.end()) {
            addSet.insert(*it);
        }
    }
}
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

int RIBNHGEntry::createSonicNHGObjFromRIBEntry(SonicGateWayNHGObject &sonicNhgOut) {
    switch (getSonicObjType()) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY:
            createSRv6GatewayObjFromRIBEntry(sonicNhgOut);
            return 0;
        default:
            SWSS_LOG_ERROR("Unsupported SonicGateWayNHGObject type: %d", getSonicObjType());
            return -1;
    }
    return -1;
}

int RIBNHGEntry::createSRv6GatewayObjFromRIBEntry(SonicGateWayNHGObject &sonicNhgOut) {
    sonicNhgOut.groupMember.clear();
    sonicNhgOut.type = SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY;
    sonicNhgOut.id = getSonicObjID();

    for (auto it = m_group.begin(); it != m_group.end(); it++) {
        RIBNHGEntry *memberEntry = m_table->getEntry(it->first);
        if (memberEntry == nullptr) {
            SWSS_LOG_ERROR("RIBNHGEntry is not exist: %d", it->first);
            return -1;
        }
        if (memberEntry->hasSonicObj() && memberEntry->getSonicObjType() == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
            if (memberEntry->getSonicGatewayObjID() != 0) {
                sonicNhgOut.groupMember.push_back(std::make_pair(memberEntry->getSonicGatewayObjID(), it->second));
            }
        }
    }

    if (sonicNhgOut.groupMember.size() == 0) {
        sonicNhgOut.nexthop = m_nexthop;
        // just for now
        sonicNhgOut.segSrc = "1::1";
        sonicNhgOut.vpnSid = m_vpnSid;
        sonicNhgOut.ifName = "unknown";
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

/* setter of private nexthopgroupfull */
void RIBNHGEntry::setNHGFull(NextHopGroupFull nhg) {
    m_nhg.id = nhg.id;
    m_nhg.weight = nhg.weight;
    m_nhg.key = nhg.key;
    m_nhg.flags = nhg.flags;
    m_nhg.ifname = nhg.ifname;
    m_nhg.depends = nhg.depends;

    m_nhg.dependents = nhg.dependents;
    m_nhg.type = nhg.type;
    m_nhg.vrf_id = nhg.vrf_id;
    m_nhg.ifindex = nhg.ifindex;
    m_nhg.nh_label_type = nhg.nh_label_type;
    m_nhg.gate = nhg.gate;
    m_nhg.bh_type = nhg.bh_type;
    m_nhg.src = nhg.src;
    m_nhg.rmap_src = nhg.rmap_src;
    m_nhg.nh_srv6 = nhg.nh_srv6;
}

/* set an RIB Entry from a nexthopgroupfull */
int RIBNHGEntry::setEntry(NextHopGroupFull nhg) {
    m_key = NexthopMapKey(&nhg);
    m_rib_id = nhg.id;
    m_nhg = nhg;
    //this->setNHGFull(nhg);
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {
        // validate group member
        if (!m_table->isNHGExist(*it)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", *it);
            return -1;
        }
        m_depends.insert(*it);
    }
    for (auto it = nhg.nh_grp_full_list.begin(); it != nhg.nh_grp_full_list.end(); it++) {
        // validate group member
        if (!m_table->isNHGExist(it->id)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", it->id);
            return -1;
        }
        m_group.insert(std::make_pair(it->id, it->weight));
    }
    m_has_sonic_obj = needCreateSonicGatewayNHGObj();
    m_resolvedGroup = getResolvedGroupFromNHGFull(nhg);

    if (this->syncFvVector() != 0) {
        SWSS_LOG_ERROR("Failed to syncd fv vector for %d", nhg.id);
        return -1;
    }

    return 0;
}

/* set the sonic obj id */
void RIBNHGEntry::setSonicObjId(uint32_t id) {
    m_sonic_obj_id = id;
}

/* sync fvVector from entry */
int RIBNHGEntry::syncFvVector() {
    string nexthops = "";
    string ifnames = "";
    string weights = "";
    uint8_t af = -1;
    m_fvVector.clear();

    if (getNHGFields(m_nhg, nexthops, ifnames, weights, af) != 0) {
        SWSS_LOG_ERROR("get nhg fields failed");
        return -1;
    }
    if (nexthops.empty() && m_nhg.nh_grp_full_list.size() > 0) {
        SWSS_LOG_ERROR("nexthop is empty");
        return -1;
    }
    if (nexthops.empty()) {
        if (af == AF_INET) {
            nexthops = "0.0.0.0";
        } else if (af == AF_INET6) {
            nexthops = "::";
        } else {
            SWSS_LOG_ERROR("sync fv vector failed");
            return -1;
        }
    }
    FieldValueTuple nh("nexthop", nexthops.c_str());
    m_fvVector.push_back(nh);

    if (!ifnames.empty()) {
        FieldValueTuple ifname("ifname", ifnames.c_str());
        m_fvVector.push_back(ifname);
    }
    if (!weights.empty()) {
        FieldValueTuple wg("weight", weights.c_str());
        m_fvVector.push_back(wg);
    }
    m_nexthop = nexthops;
    m_ifName = ifnames;
    SWSS_LOG_INFO("NextHopGroup table set: nexthop[%s] ifname[%s] weight[%s]", nexthops.c_str(), ifnames.c_str(),
                  weights.c_str());
    return 0;
}

/* get fields from entry */
int RIBNHGEntry::getNHGFields(NextHopGroupFull nhg, string &nexthop, string &ifnames, string &weights, uint8_t &af) {
    if (!m_is_single) {
        return getNextHopGroupFields(nhg, nexthop, weights);

    } else {
        if (nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX || nhg.type == fib::NEXTHOP_TYPE_IPV4) {
            af = AF_INET;
            return getNextHopFields(nhg, nexthop, ifnames, AF_INET);
        }
        if (nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX || nhg.type == fib::NEXTHOP_TYPE_IPV6) {
            af = AF_INET6;
            return getNextHopFields(nhg, nexthop, ifnames, AF_INET6);
        } else {
            SWSS_LOG_ERROR("other nexthop type: %d", nhg.type);
            return 0;
        }
    }
}

/* get fields from multi nexthopgroup entry */
int RIBNHGEntry::getNextHopGroupFields(NextHopGroupFull nhg, string &nexthops, string &weights) {
    unordered_map<uint32_t, uint8_t> resolved_group = getResolvedGroupFromNHGFull(nhg);
    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        for (const auto &nh: resolved_group) {
            uint32_t id = nh.first;
            string weight = to_string(nh.second);
            if (!m_table->isNHGExist(id)) {
                SWSS_LOG_ERROR("NextHop group is incomplete: %d", id);
                return -1;
            }
            RIBNHGEntry *entry = this->m_table->getEntry(id);
            if (!nexthops.empty()) {
                nexthops += NHG_DELIMITER;
                //ifnames += NHG_DELIMITER;
                weights += NHG_DELIMITER;
                m_vpnSid += NHG_DELIMITER;
            }
            nexthops += entry->getNextHopStr();
            weights += weight;
            m_vpnSid += entry->getVPNSIDStr();
        }
    } else {
        for (const auto &nh: resolved_group) {
            uint32_t id = nh.first;
            string weight = to_string(nh.second);
            if (!m_table->isNHGExist(id)) {
                SWSS_LOG_ERROR("NextHop group is incomplete: %d", id);
                return -1;
            }
            RIBNHGEntry *entry = this->m_table->getEntry(id);
            if (!nexthops.empty()) {
                nexthops += NHG_DELIMITER;
                //ifnames += NHG_DELIMITER;
                weights += NHG_DELIMITER;
            }
            // nexthops += entry->m_nexthop.empty() ? (af == AF_INET ? "0.0.0.0" : "::") : entry->m_nexthop;
            nexthops += entry->getNextHopStr();
            weights += weight;
        }
    }
    return 0;
}

/* get fields from single nexthop entry */
int RIBNHGEntry::getNextHopFields(NextHopGroupFull nhg, string &nexthops, string &ifnames, uint8_t af) {
    if (af == AF_INET) {
        char gateway[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &nhg.gate.ipv4, gateway, INET_ADDRSTRLEN);
        nexthops = gateway;
        ifnames = nhg.ifname;
    }
    if (af == AF_INET6) {
        char gateway[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &nhg.gate.ipv6, gateway, INET6_ADDRSTRLEN);
        nexthops = gateway;
        ifnames = nhg.ifname;
    }
    return 0;
}

/* get resolved group from nexthopgroupfull */
unordered_map<uint32_t, uint8_t> RIBNHGEntry::getResolvedGroupFromNHGFull(NextHopGroupFull nhg) {
    int preNumDirect = 0;
    unordered_map<uint32_t, uint8_t> ret;

    if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_NORMAL) {
        for (auto nhg: nhg.nh_grp_full_list) {
            if (nhg.num_direct == 0) {
                ret.insert(std::make_pair(nhg.id, nhg.weight));
                if (preNumDirect != 0) {
                    preNumDirect--;
                }
            } else {
                preNumDirect = nhg.num_direct;
            }
        }
    } else if (m_sonic_obj_type == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        for (auto nhg: nhg.nh_grp_full_list) {
            if (m_depends.find(nhg.id) != m_depends.end()) {
                ret.insert(std::make_pair(nhg.id, nhg.weight));
            }
        }
    }
    return ret;
}

bool RIBNHGEntry::needCreateSonicGatewayNHGObj() {

    /* now only for srv6 vpn gateway NHG obj */

    /* for srv6 gateway, check if we need to create pic group obj */
    for (auto it = m_group.begin(); it != m_group.end(); it++) {
        RIBNHGEntry *entry = m_table->getEntry(it->first);
        if (entry->hasSonicObj()) {
            m_sonic_obj_type = entry->getSonicObjType();
            m_is_single = false;
            return true;
        }
    }

    /* for srv6 gateway, check if we need to create single pic group obj */
    if (m_depends.size() != 0 && m_nhg.nh_srv6 != nullptr && m_nhg.nh_srv6->seg6_segs != nullptr) {
        char sid[INET6_ADDRSTRLEN] = {0};
        m_sonic_obj_type = SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY;
        inet_ntop(AF_INET6, &m_nhg.nh_srv6->seg6_segs->seg[0], sid, INET6_ADDRSTRLEN);
        m_vpnSid = sid;
        m_is_single = true;
        return true;
    }

    if (m_depends.size() > 1) {
        m_is_single = false;
    }
    return false;
}

bool RIBNHGEntry::hasSonicObj() {
    return m_has_sonic_obj;
}

sonicNhgObjType RIBNHGEntry::getSonicObjType() {
    return m_sonic_obj_type;
}

string RIBNHGEntry::getVPNSIDStr() {
    return m_vpnSid;
}

uint32_t RIBNHGEntry::getSonicGatewayObjID() {
    return m_sonic_gateway_nhg_id;
}

void RIBNHGEntry::setSonicGatewayObjId(uint32_t id) {
    m_sonic_gateway_nhg_id = id;
}

string RIBNHGEntry::getInterfaceNameStr() {
    return m_ifName;
}

NexthopMapKey::NexthopMapKey(const NextHopGroupFull *nhg) {
    //int key = nhg.id;
    /*for (int i = 0; i < nhg->depends.size(); i++)
	{
		if (i == 0)
		{
			key = key + "group:";
		}
		key = key + "id" + nhg->depends[i].id + "weight" + nhg->depends[i].weight;
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

SonicNHGTable::SonicNHGTable(RedisPipeline *pipeline, const std::string &picTableName, bool isStateTable) : m_pic_contextTable(pipeline, picTableName, isStateTable) {
}

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


void SonicNHGTable::removeFromDB(SonicGateWayNHGEntry *entry) {
    if (entry->getSonicGateWayObjType() == SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY) {
        m_pic_contextTable.del(std::to_string(entry->getSonicGateWayObjID()));
    }
    return;
}

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

int SonicNHGTable::updateEntry(SonicGateWayNHGObject sonicObj) {
    SonicGateWayNHGObjectKey key = SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(sonicObj);
    SonicGateWayNHGEntry *entry = getEntry(sonicObj.type, sonicObj.id);
    if (entry == nullptr || entry->getType() != sonicObj.type || m_sonic_nhg_map.find(key) != m_sonic_nhg_map.end()) {
        SWSS_LOG_WARN("SonicGateWayNHGObject update error");
        return -1;
    }
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

void SonicNHGTable::cleanUp() {
    for (auto it = m_sonic_nhg_map.begin(); it != m_sonic_nhg_map.end(); it++) {
        SonicGateWayNHGEntry *entry = m_sonic_nhg_map[it->first];
        delete entry;
    }
}

SonicGateWayNHGEntry *SonicNHGTable::getEntry(SonicGateWayNHGObjectKey key) {
    if (m_sonic_nhg_map.find(key) != m_sonic_nhg_map.end()) {
        return m_sonic_nhg_map[key];
    }
    return nullptr;
}

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
    return nullptr;
}

SonicGateWayNHGEntry *SonicGateWayNHGEntry::createSonicGateWayNHGEntry(SonicNHGTable *mTable) {
    SonicGateWayNHGEntry *entry = new SonicGateWayNHGEntry(mTable);
    return entry;
}

vector<FieldValueTuple> SonicGateWayNHGEntry::getFvVector() {
    return m_fvVector;
}

int SonicGateWayNHGEntry::syncFvVector() {
    m_fvVector.clear();
    switch (m_sonic_obj.type) {
        case SONIC_NHG_OBJ_TYPE_NHG_SRV6_GATEWAY: {
            if (m_sonic_obj.groupMember.size() == 0) {
                m_fvVector.push_back(FieldValueTuple("nexthop", m_sonic_obj.nexthop));
                m_fvVector.push_back(FieldValueTuple("ifname", m_sonic_obj.ifName));
                m_fvVector.push_back(FieldValueTuple("seg_src", m_sonic_obj.segSrc));
                m_fvVector.push_back(FieldValueTuple("vpn_sid", m_sonic_obj.vpnSid));
            } else {
                string nexthops = "";
                string vpnSids = "";
                string ifnames = "";
                string weights = "";
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
                    ifnames += nhg.ifName;
                    if (!vpnSids.empty()) {
                        vpnSids += NHG_DELIMITER;
                    }
                    vpnSids += nhg.vpnSid;
                    if (!weights.empty()) {
                        weights += NHG_DELIMITER;
                    }
                    weights += std::to_string(member.second);
                }
                m_fvVector.push_back(FieldValueTuple("nexthop", nexthops));
                m_fvVector.push_back(FieldValueTuple("ifname", ifnames));
                m_fvVector.push_back(FieldValueTuple("weight", weights));
                m_fvVector.push_back(FieldValueTuple("vpn_sid", vpnSids));
            }
            break;
        }
        default: {
            SWSS_LOG_ERROR("Unsupported SonicGateWayNHGObject type: %d", m_sonic_obj.type);
            return -1;
        }
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

int SonicIDAllocator::recoverSonicIDMapFromDB() {
    return 0;
}

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

int SonicGateWayNHGObjectKey::createSonicGateWayNHGObjectKey(RIBNHGEntry *entry, SonicGateWayNHGObjectKey &key_out) {
    SonicGateWayNHGObject obj;
    int ret = entry->createSonicNHGObjFromRIBEntry(obj);
    if (ret != 0) {
        return ret;
    }
    key_out = createSonicGateWayNHGObjectKey(obj);
    return 0;
}

bool operator==(const SonicGateWayNHGObjectKey &a, const SonicGateWayNHGObjectKey &b) {
    if (a.type != b.type) {
        return false;
    }
    if (a.nexthop != b.nexthop) {
        return false;
    }
    if (a.vpnSid != b.vpnSid) {
        return false;
    }
    if (a.segSrc != b.segSrc) {
        return false;
    }
    if (a.groupMember.size() != b.groupMember.size()) {
        return false;
    }
    vector<std::pair<uint32_t, uint32_t>> groupMemberA = a.groupMember;
    vector<std::pair<uint32_t, uint32_t>> groupMemberB = b.groupMember;
    sort(groupMemberA.begin(), groupMemberA.end());
    sort(groupMemberB.begin(), groupMemberB.end());
    if (groupMemberA != groupMemberB) {
        return false;
    }
    return true;
}

bool operator!=(const SonicGateWayNHGObjectKey &a, const SonicGateWayNHGObjectKey &b) {
    return !(a == b);
}