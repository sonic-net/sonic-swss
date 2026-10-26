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
    // Assigns labels to objects starting from `start_label`. SINGLE_TYPE callers
    // omit the argument so labels reset to 1 per group (legacy behavior).
    // MIXED_TYPE callers pass the profile-owned next-label so labels stay unique
    // across groups within a profile.
    void updateObjects(const std::set<std::string> &object_names, sai_uint16_t start_label = 1);
    // MIXED_TYPE variant of updateObjects(): a name already tracked by this
    // group keeps its existing label; only names not currently tracked
    // consume a new label, taken sequentially from `start_label`. Used
    // instead of updateObjects() so an update that only adds or removes a
    // handful of objects doesn't reallocate labels for the whole group -
    // in MIXED mode labels are never reused for the profile's lifetime, so
    // reallocating unchanged objects burns through the 15-bit label space
    // on every update. Returns the number of newly-allocated labels, so the
    // caller can advance its own next-label counter by exactly that amount.
    size_t updateObjectsPreservingLabels(const std::set<std::string> &object_names, sai_uint16_t start_label);
    void updateStatsIDs(std::set<sai_stat_id_t> &&stats_ids);
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
