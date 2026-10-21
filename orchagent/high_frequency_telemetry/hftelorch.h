#pragma once

#include <saitypes.h>
#include <orch.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include "counternameupdater.h"
#include "hftelprofile.h"


class HFTelOrch : public Orch
{
public:
    HFTelOrch(
        swss::DBConnector *cfg_db,
        swss::DBConnector *state_db,
        const std::vector<std::string> &tables);
    ~HFTelOrch();
    HFTelOrch(const HFTelOrch &) = delete;
    HFTelOrch &operator=(const HFTelOrch &) = delete;
    HFTelOrch(HFTelOrch &&) = delete;
    HFTelOrch &operator=(HFTelOrch &&) = delete;

    static const std::unordered_map<std::string, sai_object_type_t> SUPPORT_COUNTER_TABLES;

    // Mode used when the vendor SAI advertises both SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE
    // and SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE. MIXED_TYPE is preferred by default
    // to exercise the shared tel_type path. When the SAI_TAM_TEL_TYPE_ATTR_MODE
    // capability probe itself is unavailable, querySupportedTelTypeModes()
    // falls back to SINGLE_TYPE directly (the SAI spec's declared default),
    // not to this constant - see its comment.
    static constexpr sai_tam_tel_type_mode_t DEFAULT_TEL_TYPE_MODE = SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE;

    void locallyNotify(const CounterNameMapUpdater::Message &msg);
    static bool isSupportedHFTel(sai_object_id_t switch_id);

private:
    static bool querySupportedTelTypeModes(
        sai_object_id_t switch_id,
        bool &single_supported,
        bool &mixed_supported,
        std::unordered_set<sai_object_type_t> &tel_type_supported_categories);

    swss::Table m_state_telemetry_session;
    swss::DBConnector m_asic_db;
    swss::NotificationConsumer* m_asic_notification_consumer = nullptr;

    std::unordered_map<std::string, std::shared_ptr<HFTelProfile>> m_name_profile_mapping;
    std::unordered_map<sai_object_type_t, std::unordered_set<std::shared_ptr<HFTelProfile>>> m_type_profile_mapping;
    CounterNameCache m_counter_name_cache;

    // Tracks "profile|group" keys currently rejected by groupTableSet()
    // with task_need_retry because the group's tel_type isn't
    // canBeUpdated() (see its comment: in MIXED mode this means the shared
    // tel_type isn't SAI_TAM_TEL_TYPE_STATE_STOP_STREAM yet; in SINGLE mode
    // it means the type's own tel_type is mid-CREATE_CONFIG). task_need_retry
    // items are re-attempted on every doTask pass, so logging unconditionally
    // here would spam the log for as long as the block lasts; this set lets
    // groupTableSet log exactly once per block instead of on every retry.
    std::unordered_set<std::string> m_group_update_blocked;

    // Tracks "profile|group" keys ("profile|" for a profile-level delete)
    // currently rejected by groupTableDel()/profileTableDel() with
    // task_need_retry because the profile is still streaming (or, like
    // m_group_update_blocked, mid-CREATE_CONFIG). Logged once per block via
    // the same insert()-returns-true-only-once pattern as
    // m_group_update_blocked, and kept as a separate set so each of
    // groupTableSet()/groupTableDel()/profileTableDel() gets its own
    // once-per-block WARN with its own wording, instead of one operation's
    // block silently suppressing another's log. hasPendingGroupChange()
    // checks both sets - a pending delete has the identical dependency on
    // reaching STOP_STREAM that a pending update does.
    std::unordered_set<std::string> m_group_delete_blocked;

    // True if m_group_update_blocked or m_group_delete_blocked still holds
    // a key for this profile, i.e. some group's configuration update or
    // delete hasn't been applied yet - both are gated by one of
    // canBeUpdated()'s overloads (canBeUpdated(type) for a group,
    // canBeUpdated() for the whole profile) and so share the same
    // dependency on the profile reaching STOP_STREAM. Used at config-ready
    // time to decide whether the profile may advance to its configured
    // stream state or must first let that update/delete commit - see
    // doTask(NotificationConsumer&).
    bool hasPendingGroupChange(const std::string &profile_name) const;

    // True only when a config-ready notification for this profile must
    // settle at STOP_STREAM instead of the profile's configured state (see
    // notifyConfigReady's hold_before_configured in doTask(NotificationConsumer&)).
    // Gated on MIXED mode: hasPendingGroupChange() isn't type-scoped, and in
    // SINGLE mode each group's tel_type is independent, so an unrelated
    // group still blocked says nothing about this one - holding it anyway
    // would strand it at STOP_STREAM with no later notification to advance
    // it, since its own commit has already completed.
    bool shouldHoldBeforeConfigured(const std::shared_ptr<HFTelProfile> &profile) const;

    task_process_status profileTableSet(const std::string &profile_name, const std::vector<swss::FieldValueTuple> &values);
    task_process_status profileTableDel(const std::string &profile_name);
    task_process_status groupTableSet(const std::string &profile_name, const std::string &group_name, const std::vector<swss::FieldValueTuple> &values);
    task_process_status groupTableDel(const std::string &profile_name, const std::string &group_name);
    std::shared_ptr<HFTelProfile> getProfile(const std::string &profile_name);
    std::shared_ptr<HFTelProfile> tryGetProfile(const std::string &profile_name);
    bool isProfileInUse(const std::shared_ptr<HFTelProfile> &profile) const;

    void doTask(swss::NotificationConsumer &consumer);
    void doTask(Consumer &consumer);

    // SAI objects
    sai_object_id_t m_sai_hostif_obj;
    sai_object_id_t m_sai_hostif_trap_group_obj;
    sai_object_id_t m_sai_hostif_user_defined_trap_obj;
    sai_object_id_t m_sai_hostif_table_entry_obj;
    sai_object_id_t m_sai_tam_transport_obj;
    sai_object_id_t m_sai_tam_collector_obj;
    sai_object_id_t m_sai_tam_obj;

    sai_tam_tel_type_mode_t m_tel_type_mode;

    // Object types whose SWITCH_ENABLE_*_STATS attribute the vendor SAI implements
    // (see querySupportedTelTypeModes).
    std::unordered_set<sai_object_type_t> m_tel_type_supported_categories;

    // SAI calls
    void createNetlinkChannel(const std::string &genl_family, const std::string &genl_group);
    void deleteNetlinkChannel();
    void createTAM();
    void deleteTAM();
};
