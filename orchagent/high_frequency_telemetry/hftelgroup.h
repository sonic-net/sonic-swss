#pragma once

#include <vector>
#include <unordered_map>
#include <set>
#include <string>

#include <saitypes.h>


class HFTelGroup
{
public:
    HFTelGroup() = delete;
    HFTelGroup(const std::string& group_name);
    ~HFTelGroup() = default;
    void updateObjects(const std::set<std::string> &object_names);
    void updateStatsIDs(const std::set<sai_stat_id_t> &stats_ids);
    bool isSameObjects(const std::set<std::string> &object_names) const;
    bool isObjectInGroup(const std::string &object_name) const;
    const std::unordered_map<std::string, sai_uint16_t>& getObjects() const { return m_objects; }
    const std::set<sai_stat_id_t>& getStatsIDs() const { return m_stats_ids; }
    std::pair<std::vector<std::string>, std::vector<std::string>> getObjectNamesAndLabels() const
    {
        std::vector<std::string> names;
        std::vector<std::string> labels;
        names.reserve(m_objects.size());
        labels.reserve(m_objects.size());
        for (const auto& obj : m_objects)
        {
            names.push_back(obj.first);
            labels.push_back(std::to_string(obj.second));
        }
        return {names, labels};
    }

private:
    std::string m_group_name;
    // Object names and label IDs
    std::unordered_map<std::string, sai_uint16_t> m_objects;
    std::set<sai_stat_id_t> m_stats_ids;
};
