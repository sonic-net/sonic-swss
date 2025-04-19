#pragma once

#include <saitypes.h>
#include <saitam.h>
#include <swss/table.h>

#include <string>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <set>
#include <string>
#include <memory>

#include "hftelgroup.h"


using CounterNameCache = std::unordered_map<sai_object_type_t, std::unordered_map<std::string, sai_object_id_t>>;

// struct HFTelGroup
// {
//     // Object names and label IDs
//     std::unordered_map<std::string, std::uint16_t> m_objects;
//     std::set<sai_stat_id_t> m_stats_ids;
// };

class HFTelProfile
{
public:
    HFTelProfile(
        const std::string &profile_name,
        sai_object_id_t sai_tam_obj,
        sai_object_id_t sai_tam_collector_obj,
        const CounterNameCache &cache);
    ~HFTelProfile();
    HFTelProfile(const HFTelProfile &) = delete;
    HFTelProfile &operator=(const HFTelProfile &) = delete;
    HFTelProfile(HFTelProfile &&) = delete;
    HFTelProfile &operator=(HFTelProfile &&) = delete;

    using sai_guard_t = std::shared_ptr<sai_object_id_t>;

    const std::string& getProfileName() const;
    void setStreamState(sai_tam_tel_type_state_t state);
    void setStreamState(sai_object_type_t object_type, sai_tam_tel_type_state_t state);
    void notifyConfigReady(sai_object_type_t object_type);
    sai_tam_tel_type_state_t getTelemetryTypeState(sai_object_type_t object_type) const;
    sai_guard_t getTAMTelTypeGuard(sai_object_id_t tam_tel_type_obj) const;
    sai_object_type_t getObjectType(sai_object_id_t tam_tel_type_obj) const;
    void setPollInterval(std::uint32_t poll_interval);
    void setBulkSize(std::uint32_t bulk_size);
    void setObjectNames(const std::string &group_name, std::set<std::string> &&object_names);
    void setStatsIDs(const std::string &group_name, const std::set<std::string> &object_counters);
    void setObjectSAIID(sai_object_type_t object_type, const char *object_name, sai_object_id_t object_id);
    void delObjectSAIID(sai_object_type_t object_type, const char *object_name);
    bool canBeUpdated() const;
    bool canBeUpdated(sai_object_type_t object_type) const;
    bool isEmpty() const;

    const std::vector<std::uint8_t> &getTemplates(sai_object_type_t object_type) const;
    // const std::vector<std::string> getObjectNames(sai_object_type_t object_type) const;
    // const std::vector<std::uint16_t> getObjectLabels(sai_object_type_t object_type) const;
    std::pair<std::vector<std::string>, std::vector<std::string>> getObjectNamesAndLabels(sai_object_type_t object_type) const;
    std::vector<sai_object_type_t> getObjectTypes() const;

    // void loadGroupFromCfgDB(swss::Table &group_tbl);
    void loadCounterNameCache(sai_object_type_t object_type);
    bool tryCommitConfig(sai_object_type_t object_type);

private:
    // Configuration parameters
    const std::string m_profile_name;
    sai_tam_tel_type_state_t m_setting_state;
    std::uint32_t m_poll_interval;
    std::map<sai_object_type_t, HFTelGroup> m_groups;

    // Runtime parameters
    const CounterNameCache &m_counter_name_cache;

    std::unordered_map<
        sai_object_type_t,
        std::unordered_map<
            std::string,
            sai_object_id_t>>
        m_name_sai_map;

    // SAI objects
    const sai_object_id_t m_sai_tam_obj;
    const sai_object_id_t m_sai_tam_collector_obj;
    std::unordered_map<
        sai_object_type_t,
        std::unordered_map<
            sai_object_id_t,
            std::unordered_map<
                sai_stat_id_t,
                sai_guard_t>>>
        m_sai_tam_counter_subscription_objs;
    sai_guard_t m_sai_tam_telemetry_obj;
    std::unordered_map<sai_guard_t, sai_tam_tel_type_state_t> m_sai_tam_tel_type_states;
    std::unordered_map<sai_object_type_t, sai_guard_t> m_sai_tam_tel_type_objs;
    std::unordered_map<sai_object_type_t, sai_guard_t> m_sai_tam_report_objs;
    std::unordered_map<sai_object_type_t, std::vector<std::uint8_t>> m_sai_tam_tel_type_templates;

    bool isObjectTypeInProfile(sai_object_type_t object_type, const std::string &object_name) const;
    bool isMonitoringObjectReady(sai_object_type_t object_type) const;

    // SAI calls
    sai_object_id_t getTAMReportObjID(sai_object_type_t object_type);
    sai_object_id_t getTAMTelTypeObjID(sai_object_type_t object_type);
    void initTelemetry();
    void deployCounterSubscription(sai_object_type_t object_type, sai_object_id_t sai_obj, sai_stat_id_t stat_id, std::uint16_t label);
    void deployCounterSubscriptions(sai_object_type_t object_type, sai_object_id_t sai_obj, std::uint16_t label);
    void deployCounterSubscriptions(sai_object_type_t object_type);
    void undeployCounterSubscriptions(sai_object_type_t object_type);
    void updateTemplates(sai_object_id_t tam_tel_type_obj);
};
