#pragma once

#include <unordered_map>
#include <set>
#include <string>

#include <saitypes.h>


class HFTelGroup
{
public:
    HFTelGroup(const std::string& group_name);
    ~HFTelGroup() = default;
    void updateObjects(const std::set<std::string> &object_names);
    void updateStatsIDs(const std::set<sai_stat_id_t> &stats_ids);
    bool isSameObjects(const std::set<std::string> &object_names) const;
    bool isObjectInGroup(const std::string &object_name) const;

    private:
    std::string m_group_name;
    // Object names and label IDs
    std::unordered_map<std::string, sai_uint16_t> m_objects;
    std::set<sai_stat_id_t> m_stats_ids;

public:
    const std::unordered_map<std::string, sai_uint16_t>& m_objects_ref;
    const std::set<sai_stat_id_t>& m_stats_ids_ref;
};
