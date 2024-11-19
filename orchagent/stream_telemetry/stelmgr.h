#pragma once

#include <saitypes.h>
#include <swss/table.h>

#include <string>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include <set>
#include <string>
#include <memory>

using CounterNameCache = std::unordered_map<sai_object_type_t, std::unordered_map<std::string, sai_object_id_t>>;

struct StelGroup
{
    std::set<std::string> m_object_names;
    std::set<sai_stat_id_t> m_stats_ids;
};

struct StelEntry
{
    std::uint16_t m_label;
    sai_object_id_t m_object_id;
};

class STelProfile
{
public:
    enum STREAM_STATE
    {
        STREAM_STATE_ENABLED,
        STREAM_STATE_DISABLED,
    };

    STelProfile(
        const std::string &profile_name,
        sai_object_id_t sai_tam_obj,
        sai_object_id_t sai_tam_collector_obj,
        const CounterNameCache &cache);
    ~STelProfile();
    STelProfile(const STelProfile &) = delete;
    STelProfile &operator=(const STelProfile &) = delete;
    STelProfile(STelProfile &&) = delete;
    STelProfile &operator=(STelProfile &&) = delete;

    void setStreamState(STREAM_STATE state);
    void setPollInterval(std::uint32_t poll_interval);
    void setBulkSize(std::uint32_t bulk_size);
    void setObjectNames(const std::string &group_name, std::set<std::string> &&object_names);
    void setStatsIDs(const std::string &group_name, const std::set<std::string> &object_counters);
    void setObjectSAIID(sai_object_type_t object_type, const char *object_name, sai_object_id_t object_id);
    void delObjectSAIID(sai_object_type_t object_type, const char *object_name);

    std::vector<std::uint8_t> getTemplates() const;
    std::vector<sai_object_type_t> getObjectTypes() const;

    void loadGroupFromCfgDB(swss::Table &group_tbl);
    void loadCounterNameCache(sai_object_type_t object_type);
    void tryCommitConfig();

    static sai_object_type_t group_name_to_sai_type(const std::string &group_name);

private:
    // Configuration parameters
    const std::string m_profile_name;
    STREAM_STATE m_setting_state;
    std::uint32_t m_poll_interval;
    std::uint32_t m_bulk_size;
    std::map<sai_object_type_t, StelGroup> m_groups;

    // Runtime parameters
    const CounterNameCache &m_counter_name_cache;
    STREAM_STATE m_state;
    size_t m_object_count;
    std::unordered_map<
        sai_object_type_t,
        std::unordered_map<
            std::string,
            sai_object_id_t>>
        m_name_sai_map;
    bool m_needed_to_be_deployed;

    // SAI objects
    const sai_object_id_t m_sai_tam_obj;
    const sai_object_id_t m_sai_tam_collector_obj;
    sai_object_id_t m_sai_tam_report_obj;
    sai_object_id_t m_sai_tam_tel_type_obj;
    sai_object_id_t m_sai_tam_telemetry_obj;
    using counter_subscription_t = std::unique_ptr<sai_object_id_t, std::function<void(sai_object_id_t *)>>;
    std::unordered_map<
        sai_object_id_t,
        std::unordered_map<
            sai_stat_id_t,
            counter_subscription_t>>
        m_sai_tam_counter_subscription_objs;

    bool isObjectInProfile(sai_object_type_t object_type, const std::string &object_name) const;

    void deployToSAI();
    void undeployFromSAI();

    // SAI calls
    void deployTAM();
    void undeployTAM();
    void deployCounterSubscription(sai_object_type_t object_type, sai_object_id_t sai_obj, sai_stat_id_t stat_id, std::uint16_t label);
};
