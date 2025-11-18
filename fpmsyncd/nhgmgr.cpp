#include "nhgmgr.h"
#include "logger.h"
#include <string.h>
#define NHG_DELIMITER ','

using namespace std;
using namespace swss;

NHGMgr::NHGMgr(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable) {
    m_rib_nhg_table = new RIBNHGTable(pipeline, tableName, isStateTable);
}

int NHGMgr::addNHGFull(NextHopGroupFull nhg) {
    int ret = 0;
    if (m_rib_nhg_table->isNHGExist(nhg.id)) {
        ret = m_rib_nhg_table->updateEntry(nhg);
    } else {
        ret = m_rib_nhg_table->addEntry(nhg);
        RIBNHGEntry *entry = m_rib_nhg_table->getEntry(nhg.id);
        if (entry == nullptr) {
            return -1;
        }
        uint32_t sonic_id = m_sonic_id_manager.allocateID();
        entry->setSonicObjId(sonic_id);
        if (m_rib_nhg_table->writeToDB(entry) != 0) {
            m_rib_nhg_table->delEntry(nhg.id);
            m_sonic_id_manager.freeID(sonic_id);
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return -1;
        }
    }


    // TODO: srv6 sonic nhg create
    return ret;
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
    uint32_t sonic_id = entry->getSoincObjID();
    if (m_rib_nhg_table->delEntry(id) != 0) {
        return -1;
    }
    m_sonic_id_manager.freeID(sonic_id);
    return 0;
}

void NHGMgr::dump_zebra_nhg_table(string &ret) {
    m_rib_nhg_table->dump_table(ret);
}

/*
bool NHGMgr::getIfName(int if_index, char *if_name, size_t name_len)
{
    if (!if_name || name_len == 0)
    {
        return false;
    }

    memset(if_name, 0, name_len);

    if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
    {
        nl_cache_refill(m_nl_sock, m_link_cache);
        if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
        {
            return false;
        }
    }

    return true;
}
*/

RIBNHGEntry *NHGMgr::getRIBNHGEntryByKey(string key) {
    return NULL;
}

RIBNHGEntry *NHGMgr::getRIBNHGEntryByRIBID(uint32_t id) {
    return m_rib_nhg_table->getEntry(id);
}

// TODO: add sonic object creation
SonicNHGObject *NHGMgr::getSonicNHGByKey(std::string key) {
    return nullptr;
}

SonicNHGObject *NHGMgr::getSonicNHGByID(uint32_t id) {
    return nullptr;
}

RIBNHGTable::RIBNHGTable(RedisPipeline *pipeline, const std::string &tableName, bool isStateTable) : m_nexthop_groupTable(pipeline, tableName, isStateTable) {
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

uint32_t RIBNHGEntry::getSoincObjID() {
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

    RIBNHGEntry *entry = RIBNHGEntry::createNHGEntry(nhg, this);
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

int RIBNHGTable::updateEntry(NextHopGroupFull nhg) {
    if (m_nhg_map.find(nhg.id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not exists.", nhg.id);
        return -1;
    }

    auto it = m_nhg_map.find(nhg.id);
    RIBNHGEntry *entry = it->second;

    bool updated = false, updatedDependency = false;
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

    if (updated) {
        ret = writeToDB(entry);
        if (ret < 0) {
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return -1;
        }
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

    vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        SWSS_LOG_ERROR("Failed to sync fvVector for %d, empty fvVector", entry->getNHG().id);
        return -1;
    }
    m_nexthop_groupTable.set(std::to_string(entry->getSoincObjID()), fvVector);
    return 0;
}

void RIBNHGTable::removeFromDB(RIBNHGEntry *entry) {
    m_nexthop_groupTable.del(std::to_string(entry->getSoincObjID()));
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

void RIBNHGTable::diffDependency(set<uint32_t> oldSet, set<uint32_t> newSet, set<uint32_t> &addSet, set<uint32_t> &removeSet) {
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

RIBNHGEntry::RIBNHGEntry(RIBNHGTable *table, NextHopGroupFull nhg) : m_table(table), m_key(&nhg), m_nhg(nhg) {
}

RIBNHGEntry::~RIBNHGEntry() {
}

/* static creation func */
RIBNHGEntry *RIBNHGEntry::createNHGEntry(NextHopGroupFull nhg, RIBNHGTable *mTable) {
    RIBNHGEntry *entry = new RIBNHGEntry(mTable, nhg);
    return entry;
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
    return m_resolved_group;
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
NexthopKey RIBNHGEntry::getKey() {
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
    m_key = NexthopKey(&nhg);
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

    m_resolved_group = getResolvedGroupFromNHGFull(nhg);

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
        return -1;
    }
    if (nexthops.empty()) {
        if (af == AF_INET) {
            nexthops = "0.0.0.0";
        } else if (af == AF_INET6) {
            nexthops = "::";
        } else {
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
    SWSS_LOG_INFO("NextHopGroup table set: nexthop[%s] ifname[%s] weight[%s]", nexthops.c_str(), ifnames.c_str(), weights.c_str());
    return 0;
}


/* get fields from entry */
int RIBNHGEntry::getNHGFields(NextHopGroupFull nhg, string &nexthop, string &ifnames, string &weights, uint8_t &af) {
    if (nhg.nh_grp_full_list.size() > 0) {
        return getNextHopGroupFields(nhg, nexthop, weights);

    } else {
        if (nhg.type == fib::NEXTHOP_TYPE_IPV4_IFINDEX || nhg.type == fib::NEXTHOP_TYPE_IPV4) {
            af = AF_INET;
            return getNextHopFields(nhg, nexthop, ifnames, AF_INET);
        }
        if (nhg.type == fib::NEXTHOP_TYPE_IPV6_IFINDEX || nhg.type == fib::NEXTHOP_TYPE_IPV6) {
            af = AF_INET6;
            return getNextHopFields(nhg, nexthop, ifnames, AF_INET6);
        }
        return -1;
    }
}

/* get fields from multi nexthopgroup entry */
int RIBNHGEntry::getNextHopGroupFields(NextHopGroupFull nhg, string &nexthops, string &weights) {
    unordered_map<uint32_t, uint8_t> resolved_group = getResolvedGroupFromNHGFull(nhg);
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
        nexthops += to_string(entry->getSoincObjID());
        weights += weight;
    }
    return 0;
}

/* get fields from single nexthop entry */
int RIBNHGEntry::getNextHopFields(NextHopGroupFull nhg, string &nexthops, string &ifnames, uint8_t af) {
    if (nhg.nh_grp_full_list.size() == 0) {
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
    } else {
        return -1;
    }
}

/* get resolved group from nexthopgroupfull */
unordered_map<uint32_t, uint8_t> RIBNHGEntry::getResolvedGroupFromNHGFull(NextHopGroupFull nhg) {
    int preNumDirect = 0;
    unordered_map<uint32_t, uint8_t> ret;
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
    return ret;
}

NexthopKey::NexthopKey(const NextHopGroupFull *nhg) {
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

int SonicNHGTable::addEntry() {
    return 0;
}

int SonicNHGTable::delEntry() {
    return 0;
}

SonicNHGEntry *SonicNHGTable::getEntry(std::string key) {
    return nullptr;
}

SonicNHGEntry *SonicNHGTable::getEntry(uint32_t id) {
    return nullptr;
}

void SonicNHGEntry::createNHGEntry() {
    return;
}
