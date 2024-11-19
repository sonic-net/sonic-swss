#pragma once

#include <saitypes.h>
#include <orch.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include "counternameupdater.h"
#include "stelmgr.h"

#define CFG_STREAM_TELEMETRY_PROFILE_TABLE_NAME "STREAM_TELEMETRY_PROFILE"
#define CFG_STREAM_TELEMETRY_GROUP_TABLE_NAME "STREAM_TELEMETRY_GROUP"
#define STREAM_TELEMETRY_SESSION "STREAM_TELEMETRY_SESSION"

class STelOrch : public Orch
{
public:
    STelOrch(
        swss::DBConnector *cfg_db,
        swss::DBConnector *state_db,
        const std::vector<std::string> &tables);
    ~STelOrch();
    STelOrch(const STelOrch &) = delete;
    STelOrch &operator=(const STelOrch &) = delete;
    STelOrch(STelOrch &&) = delete;
    STelOrch &operator=(STelOrch &&) = delete;

    static const std::unordered_map<std::string, sai_object_type_t> SUPPORT_COUNTER_TABLES;

    void locallyNotify(const CounterNameMapUpdater::Message &msg);

private:
    swss::Table m_cfg_stream_telemetry_group;
    swss::Table m_state_telemetry_session;

    std::unordered_map<std::string, std::shared_ptr<STelProfile>> m_name_profile_mapping;
    std::unordered_map<sai_object_type_t, std::unordered_set<std::shared_ptr<STelProfile>>> m_type_profile_mapping;
    CounterNameCache m_counter_name_cache;

    void profileTableSet(const std::string &profile_name, const std::vector<swss::FieldValueTuple> &values);
    void profileTableDel(const std::string &profile_name);
    void groupTableSet(const std::string &profile_name, const std::string &group_name, const std::vector<swss::FieldValueTuple> &values);
    void groupTableDel(const std::string &profile_name, const std::string &group_name);
    std::shared_ptr<STelProfile> getProfile(const std::string &profile_name);

    void doTask(Consumer &consumer);

    // SAI objects
    sai_object_id_t m_sai_hostif_obj;
    sai_object_id_t m_sai_hostif_trap_group_obj;
    sai_object_id_t m_sai_hostif_user_defined_trap_obj;
    sai_object_id_t m_sai_hostif_table_entry_obj;
    sai_object_id_t m_sai_tam_transport_obj;
    sai_object_id_t m_sai_tam_collector_obj;
    sai_object_id_t m_sai_tam_obj;

    // SAI calls
    void createNetlinkChannel(const std::string &genl_family, const std::string &genl_group);
    void deleteNetlinkChannel();
    void createTAM();
    void deleteTAM();
};
