#include "nhgmgr.h"
#include "logger.h"
#include <string.h>
#define NHG_DELIMITER ','

using namespace std;
using namespace swss;

NHGMgr::NHGMgr(RedisPipeline *pipeline, const std::string &tableName, bool is_state_table) {
    m_rib_nhg_table = new RIBNhgTable(pipeline, tableName, is_state_table);
}

int NHGMgr::addNHGFull(NextHopGroupFull nhg) {
    int ret = 0;
    if (m_rib_nhg_table->isNhgExist(nhg.id)) {
        ret = m_rib_nhg_table->updateNhg(nhg);
    } else {
        ret = m_rib_nhg_table->addNhg(nhg);
    }
    // TODO: srv6 sonic nhg create
    return ret;
}

int NHGMgr::delNHGFull(uint32_t id) {
    if (!m_rib_nhg_table->isNhgExist(id)) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    // TODO: del nhg from sonic table
    return m_rib_nhg_table->delNhg(id);
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

RIBNhgEntry *NHGMgr::getRIBNhgEntryByKey(string key) {
    return NULL;
}

RIBNhgEntry *NHGMgr::getRIBNhgEntryByRIBID(uint32_t id) {
    return m_rib_nhg_table->getEntry(id);
}

RIBNhgEntry::~RIBNhgEntry() {
    //delete m_nhg;
}
// TODO: add sonic object creation
SonicNHGObject *NHGMgr::getSonicNHGByKey(std::string key) {
    return nullptr;
}

SonicNHGObject *NHGMgr::getSonicNHGByID(uint32_t id) {
    return nullptr;
}
void NHGMgr::dump_zebra_nhg_table(string &ret) {
    m_rib_nhg_table->dump_table(ret);
}

RIBNhgTable::RIBNhgTable(RedisPipeline *pipeline, const std::string &tableName, bool is_state_table) : m_nexthop_groupTable(pipeline, tableName, is_state_table) {
}
/*
RIBNhgEntry *RIBNhgTable::getEntry(string key)
{
    auto it = m_key_2_id_map.find(key);
    if (it == m_key_2_id_map.end())
    {
        return nullptr;
    }
    it = m_nhg_map.find(it->second);
    if (it == m_nhg_map.end())
    {
        return nullptr;
    }
    return it->second;
}
*/
RIBNhgEntry *RIBNhgTable::getEntry(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it == m_nhg_map.end()) {
        return nullptr;
    }
    return it->second;
}

int RIBNhgEntry::updateEntryFromNHGFull(NextHopGroupFull new_nhg, bool &updated) {
    if (new_nhg.weight != m_nhg.weight) {
        m_nhg.weight = new_nhg.weight;
        updated = true;
    }
    if (new_nhg.vrf_id != m_nhg.vrf_id) {
        m_nhg.vrf_id = new_nhg.vrf_id;
        updated = true;
    }
    if (new_nhg.ifindex != m_nhg.ifindex) {
        m_nhg.ifindex = new_nhg.ifindex;
        updated = true;
    }
    if (new_nhg.ifname != m_nhg.ifname) {
        m_nhg.ifname = new_nhg.ifname;
        updated = true;
    }
    if (!compareDependsAndDependents(&new_nhg, &m_nhg)) {
        m_nhg.depends = new_nhg.depends;
        m_nhg.dependents = new_nhg.dependents;
        updated = true;
    }
    if (updated) {
        int ret = this->setEntry(m_nhg);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

bool RIBNhgEntry::compareDependsAndDependents(const NextHopGroupFull *new_nhg, const NextHopGroupFull *old_nhg) {
    if ((new_nhg->depends.size()) != (old_nhg->depends.size())) {
        return false;
    }
    if ((new_nhg->dependents.size()) != (old_nhg->dependents.size())) {
        return false;
    }
    map<int, nh_grp_full> new_nhg_depends;
    map<int, nh_grp_full> old_nhg_depends;
    map<int, nh_grp_full> new_nhg_dependents;
    map<int, nh_grp_full> old_nhg_dependents;
    for (auto it = new_nhg->depends.begin(); it != new_nhg->depends.end(); it++) {
        new_nhg_depends[it->id] = *it;
    }
    for (auto it = new_nhg->dependents.begin(); it != new_nhg->dependents.end(); it++) {
        new_nhg_dependents[it->id] = *it;
    }
    for (auto it = old_nhg->depends.begin(); it != old_nhg->depends.end(); it++) {
        old_nhg_depends[it->id] = *it;
        if (new_nhg_depends
                    .find(it->id) == new_nhg_depends.end()) {
            return false;
        }
        if (new_nhg_depends[it->id].weight != it->weight) {
            return false;
        }
        if (new_nhg_depends[it->id].num_direct != it->num_direct) {
            return false;
        }
    }
    for (auto it = old_nhg->dependents.begin(); it != old_nhg->dependents.end(); it++) {
        old_nhg_dependents[it->id] = *it;
        if (new_nhg_dependents
                    .find(it->id) == new_nhg_dependents.end()) {
            return false;
        }
        if (new_nhg_dependents
                    [it->id]
                            .weight != it->weight) {
            return false;
        }
        if (new_nhg_dependents
                    [it->id]
                            .num_direct != it->num_direct) {
            return false;
        }
    }
    for (auto it = new_nhg->depends.begin(); it != new_nhg->depends.end(); it++) {
        if (old_nhg_depends
                    .find(it->id) == old_nhg_depends.end()) {
            return false;
        }
    }
    for (auto it = new_nhg->dependents.begin(); it != new_nhg->dependents.end(); it++) {
        if (old_nhg_dependents
                    .find(it->id) == old_nhg_dependents.end()) {
            return false;
        }
    }
    return true;
}


int RIBNhgTable::delNhg(uint32_t id) {
    if (m_nhg_map.find(id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not found.", id);
        return 0;
    }

    RIBNhgEntry *entry = m_nhg_map[id];
    if (entry->getDependentsID().size() != 0) {
        SWSS_LOG_ERROR("NextHop group id %d still has dependents.", id);
        return -1;
    }
    this->removeFromDB(entry);
    // TODO: update depends and dependents

    //m_key_2_id_map.erase(entry->getKey());
    delete entry;
    m_nhg_map.erase(id);
    return 0;
}

int RIBNhgTable::addNhg(NextHopGroupFull nhg) {
    if (m_nhg_map.find(nhg.id) != m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d already exists.", nhg.id);
        return -1;
    }

    RIBNhgEntry *entry = RIBNhgEntry::create_nhg_entry(nhg, this);
    if (entry == nullptr) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }
    if (writeToDB(entry) != 0) {
        SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
        return -1;
    }

    // TODO: update depends and dependents

    m_nhg_map.insert(std::make_pair(nhg.id, entry));
    //m_key_2_id_map.insert(std::make_pair(entry->getKey(), entry));
    return 0;
}

int RIBNhgTable::updateNhg(NextHopGroupFull nhg) {
    if (m_nhg_map.find(nhg.id) == m_nhg_map.end()) {
        SWSS_LOG_ERROR("NextHop group id %d not exists.", nhg.id);
        return -1;
    }

    auto it = m_nhg_map.find(nhg.id);
    RIBNhgEntry *entry = it->second;

    bool updated = false;
    int ret = entry->updateEntryFromNHGFull(nhg, updated);
    if (ret != 0) {
        SWSS_LOG_ERROR("Failed to create nhg entry for %d", nhg.id);
        return -1;
    }
    if (updated) {
        ret = writeToDB(entry);
        if (ret < 0) {
            SWSS_LOG_ERROR("Failed to write to DB for %d", nhg.id);
            return -1;
        }
    }

    // TODO: update depends and dependents
    //m_key_2_id_map.insert(std::make_pair(entry->getKey(), entry));
    return 0;
}

void RIBNhgTable::add_nhg_dependents(RIBNhgEntry *entry) {
}

void RIBNhgTable::remove_nhg_dependents(RIBNhgEntry *entry) {
    // TODO
    //    for (int i = 0; i < entry->getNhg().group.size(); i++)
    //   {
    //      m_nhg_map[entry->getNhg().group[i].first]->m_depends.push_back(entry->getNhg().id);
    // }
}
/*
bool RIBNhgTable::isNhgExist(string key)
{
    auto it = m_key_2_id_map.find(key);
    if (it != m_key_2_id_map.end())
    {
        return true;
    }
    return false;
}
*/
bool RIBNhgTable::isNhgExist(uint32_t id) {
    auto it = m_nhg_map.find(id);
    if (it != m_nhg_map.end()) {
        return true;
    }
    return false;
}
int RIBNhgTable::writeToDB(RIBNhgEntry *entry) {

    vector<FieldValueTuple> fvVector = entry->getFvVector();
    if (fvVector.size() == 0) {
        if (entry->syncFvVector() != 0) {
            SWSS_LOG_ERROR("Failed to sync fvVector for %d", entry->getNhg().id);
            return -1;
        }
        fvVector = entry->getFvVector();
    }
    m_nexthop_groupTable.set(std::to_string(entry->getNhg().id), fvVector);
    return 0;
}

void RIBNhgTable::removeFromDB(RIBNhgEntry *entry) {
    m_nexthop_groupTable.del(std::to_string(entry->getNhg().id));
    return;
}

void RIBNhgTable::dump_table(string &ret) {
    /* string indent = "    ";
    for (auto it = m_nhg_map.begin(); it != m_nhg_map.end(); it++)
    {
        NextHopGroupFull nhg = it->second->getNhg();
        ret += "nhg id: " + to_string(nhg.id) + ":\n";
        if (it->second->isInstalled())
        {
            ret += indent + "installed\n";
        }
        else
        {
            ret += indent + "not installed\n";
        }
        if (it->second->getGroup().size() != 0){
            ret += indent + "group: ";
            for (auto it2 = it->second->getGroup().begin(); it2 != it->second->getGroup().end(); it2++)
            {
                ret += to_string(it2->first) + ":" + to_string(it2->second) + ",";
            }
        }

        if (!it->second->getNexthop().empty())
        {
            ret += indent + "nexthop: " + it->second->getNexthop() + "\n";
        }

        switch (nhg.type)
        {
            case fib::NEXTHOP_TYPE_IFINDEX:
                ret += indent + "type: " + "fib::NEXTHOP_TYPE_IFINDEX" + "\n";
                ret += indent + "interface: " + it->second->ifindex + "\n";
                break ;
            case fib::NEXTHOP_TYPE_IPV4:
                ret += indent + "type: " + "fib::NEXTHOP_TYPE_IPV4" + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV4_IFINDEX:
                ret += indent + "type: " + "fib::NEXTHOP_TYPE_IPV4_IFINDEX" + "\n";
                ret += indent + "interface: " + it->second->ifindex + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV6_IFINDEX:
                ret += indent + "type: " + "fib::NEXTHOP_TYPE_IPV6_IFINDEX" + "\n";
                ret += indent + "interface: " + it->second->ifindex + "\n";
                break;
            case fib::NEXTHOP_TYPE_IPV6:
                ret += indent + "type: " + "fib::NEXTHOP_TYPE_IPV6" + "\n";
                break;
            default:
                SWSS_LOG_ERROR("Unknown NextHop type %d", nhg.type);
                return ;
        }
    }*/
}

RIBNhgEntry *RIBNhgEntry::create_nhg_entry(NextHopGroupFull nhg, RIBNhgTable *table) {
    RIBNhgEntry *entry = new RIBNhgEntry(table, nhg);
    int ret = entry->setEntry(nhg);
    if (ret != 0) {
        delete entry;
        return nullptr;
    }
    return entry;
}

vector<pair<uint32_t, uint8_t>> RIBNhgEntry::getGroup() {
    return m_group;
}

vector<pair<uint32_t, uint8_t>> RIBNhgEntry::getResolvedGroup() {
    return m_resolved_group;
}

vector<RIBNhgEntry *> RIBNhgEntry::getDependsID() {
    vector<RIBNhgEntry *> r;
    return r;
}

vector<RIBNhgEntry *> RIBNhgEntry::getDependentsID() {
    vector<RIBNhgEntry *> r;
    return r;
}

NexthopKey RIBNhgEntry::getKey() {
    return m_key;
}

NextHopGroupFull RIBNhgEntry::getNhg() {
    return m_nhg;
}

void RIBNhgEntry::setNHGFull(NextHopGroupFull nhg) {
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

int RIBNhgEntry::setEntry(NextHopGroupFull nhg) {
    m_key = NexthopKey(&nhg);
    this->setNHGFull(nhg);
    for (auto it = nhg.depends.begin(); it != nhg.depends.end(); it++) {

        // validate group member
        if (!m_table->isNhgExist(it->id)) {
            SWSS_LOG_ERROR("NextHop id %d in group not found.", it->id);
            return -1;
        }

        // update resolved group
        // if (nhg.depends[i].type != fib::NEXTHOP_TYPE_RECURSIVED)
        // {
        //    this->resolved_group.push_back(nhg.depends[i]);
        //}
    }
    switch (nhg.type) {
        case fib::NEXTHOP_TYPE_IFINDEX:
            m_nexthop = "";
            break;
        case fib::NEXTHOP_TYPE_IPV4: {
            char gate_str[INET_ADDRSTRLEN];
            m_af = AF_INET;
            inet_ntop(AF_INET, &nhg.gate.ipv4, gate_str, INET_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        }
        case fib::NEXTHOP_TYPE_IPV4_IFINDEX: {
            char gate_str[INET_ADDRSTRLEN];
            m_af = AF_INET;
            inet_ntop(AF_INET, &nhg.gate.ipv4, gate_str, INET_ADDRSTRLEN);
            m_nexthop = gate_str;
            break;
        }
        case fib::NEXTHOP_TYPE_IPV6_IFINDEX: {
            char gate_str[INET6_ADDRSTRLEN];
            m_af = AF_INET6;
            m_nexthop = inet_ntop(AF_INET6, &nhg.gate.ipv6, gate_str, INET6_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        }
        case fib::NEXTHOP_TYPE_IPV6: {
            char gate_str[INET6_ADDRSTRLEN];
            m_af = AF_INET6;
            inet_ntop(AF_INET6, &nhg.gate.ipv6, gate_str, INET6_ADDRSTRLEN);
            m_nexthop = string(gate_str);
            break;
        }
        default:
            SWSS_LOG_ERROR("Unknown NextHop type %d", nhg.type);
            return -1;
    }

    return 0;
}

vector<FieldValueTuple> RIBNhgEntry::getFvVector() {
    return m_fvVector;
}

int RIBNhgEntry::syncFvVector() {
    string nexthops;
    string ifnames;
    string weights;
    // TODO: sonic id manager allocate the key
    std::string key = to_string(m_nhg.id);
    ifnames = m_nhg.ifname;
    int ret = getNextHopGroupFields(nexthops, ifnames, weights, m_af);
    if (ret < 0) {
        SWSS_LOG_ERROR("Failed to get field of %d", m_nhg.id);
        return ret;
    }

    FieldValueTuple nh("nexthop", nexthops.c_str());
    FieldValueTuple ifname("ifname", ifnames.c_str());
    m_fvVector.push_back(nh);
    m_fvVector.push_back(ifname);
    if (!weights.empty()) {
        FieldValueTuple wg("weight", weights.c_str());
        m_fvVector.push_back(wg);
    }
    SWSS_LOG_INFO("NextHopGroup table set: key [%s] nexthop[%s] ifname[%s] weight[%s]", key.c_str(), nexthops.c_str(), ifnames.c_str(), weights.c_str());
    return 0;
}


int RIBNhgEntry::getNextHopGroupFields(string &nexthops, string &ifnames, string &weights, uint8_t af) {
    if (m_nhg.depends.size() == 0) {
        if (!m_nexthop.empty()) {
            nexthops = m_nexthop;
        } else {
            nexthops = af == AF_INET ? "0.0.0.0" : "::";
        }
        ifnames = m_nhg.ifname;
    } else {
        int i = 0;
        for (const auto &nh: m_nhg.depends) {
            uint32_t id = nh.id;
            string weight = to_string(nh.weight);
            if (!m_table->isNhgExist(id)) {
                SWSS_LOG_ERROR("NextHop group is incomplete: %d", nh.id);
                return -1;
            }
            RIBNhgEntry *entry = this->m_table->getEntry(nh.id);
            if (i) {
                nexthops += NHG_DELIMITER;
                ifnames += NHG_DELIMITER;
                weights += NHG_DELIMITER;
            }
            nexthops += entry->m_nexthop.empty() ? (af == AF_INET ? "0.0.0.0" : "::") : entry->m_nexthop;
            ifnames += entry->getNhg().ifname;
            weights += weight;
            ++i;
        }
    }
    return 0;
}


RIBNhgEntry::RIBNhgEntry(RIBNhgTable *table, NextHopGroupFull nhg) : m_table(table), m_key(&nhg), m_nhg(nhg) {
}

string RIBNhgEntry::getNexthop() {
    return m_nexthop;
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
    case fib::NEXTHOP_TYPE_IFINDEX:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        break ;
    }
    case fib::NEXTHOP_TYPE_IPV4:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv4;
        break ;

    }
    case fib::NEXTHOP_TYPE_IPV4_IFINDEX:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv4;
        break ;

    }
    case fib::NEXTHOP_TYPE_IPV6:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv6;
        break ;

    }
    case fib::NEXTHOP_TYPE_IPV6_IFINDEX:
    {
        key = key + "type:" + nhg->type;
        key = key + "ifindex:" + nhg->ifindex;
        key = key + "vrf_id" + nhg->vrf_id;
        key = key + "gate" + nhg->gate.ipv6;
        break ;

    }
    case fib::NEXTHOP_TYPE_BLACKHOLE:
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


int SonicNHGTable::addNhg() {
    return 0;
}

int SonicNHGTable::delNhg() {
    return 0;
}

SonicNHGEntry *SonicNHGTable::getEntry(std::string key) {
    return nullptr;
}

SonicNHGEntry *SonicNHGTable::getEntry(uint32_t id) {
    return nullptr;
}


void SonicNHGEntry::create_nhg_entry() {
    return;
}
