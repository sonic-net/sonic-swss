#ifndef DASHHAORCH_H
#define DASHHAORCH_H
#include <map>

#include "dbconnector.h"
#include "dashorch.h"
#include "bfdorch.h"
#include "zmqorch.h"
#include "zmqserver.h"
#include "saitypes.h"
#include "notifier.h"
#include "directory.h"
#include "sai_serialize.h"
#include "notifications.h"

#include "dash_api/ha_scope.pb.h"

#include "pbutils.h"

#define HA_SET_STAT_COUNTER_FLEX_COUNTER_GROUP "HA_SET_STAT_COUNTER"
#define HA_SET_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000

struct HaScopeEntry
{
    sai_object_id_t ha_scope_id;
    dash::ha_scope::HaScope metadata;
    std::time_t last_role_start_time;

    sai_dash_ha_state_t ha_state;
    std::time_t last_state_start_time;
};

typedef std::map<std::string, HaScopeEntry> HaScopeTable;
typedef std::map<std::string, vector<swss::FieldValueTuple>> DashBfdSessionTable;

template <typename T>
bool in(T value, std::initializer_list<T> list) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

class DashHaOrch : public ZmqOrch
{
public:
    DashHaOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, DashOrch *dash_orch, BfdOrch *bfd_orch, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer);

protected:
    HaSetTable m_ha_set_entries;
    HaScopeTable m_ha_scope_entries;
    DashBfdSessionTable m_bfd_session_pending_creation;

    DashOrch *m_dash_orch;
    BfdOrch *m_bfd_orch;

    void doTask(ConsumerBase &consumer);
    void doTask(swss::NotificationConsumer &consumer);
    void doTaskEniTable(ConsumerBase &consumer);
    void doTaskHaSetTable(ConsumerBase &consumer);
    void doTaskHaScopeTable(ConsumerBase &consumer);
    void doTaskBfdSessionTable(ConsumerBase &consumer);

    bool addHaSetEntry(const std::string &key, const dash::ha_set::HaSet &entry);
    bool removeHaSetEntry(const std::string &key);
    bool addHaScopeEntry(const std::string &key, const dash::ha_scope::HaScope &entry);
    bool removeHaScopeEntry(const std::string &key);
    bool setHaScopeHaRole(const std::string &key, const dash::ha_scope::HaScope &entry);
    bool setHaScopeFlowReconcileRequest(const  std::string &key);
    bool setHaScopeActivateRoleRequest(const std::string &key);
    bool setHaScopeDisabled(const std::string &key, bool disabled);
    bool setEniHaScopeId(const sai_object_id_t eni_id, const sai_object_id_t ha_scope_id);
    bool register_ha_set_notifier();
    bool register_ha_scope_notifier();
    bool updateExistingHaSetEntry(const std::string &key, const dash::ha_set::HaSet &entry, sai_object_id_t sai_ha_set_oid);

    bool has_dpu_scope();
    bool has_eni_scope();

    void processCachedBfdSessions();

    bool convertKfvToHaSetPb(
        const std::vector<swss::FieldValueTuple> &kfv,
        dash::ha_set::HaSet &entry
    );

    bool convertKfvToHaScopePb(
        const std::vector<swss::FieldValueTuple> &kfv,
        dash::ha_scope::HaScope &entry
    );

    std::string getHaSetObjectKey(const sai_object_id_t ha_set_id);
    std::string getHaScopeObjectKey(const sai_object_id_t ha_scope_id);
    std::time_t getNowTime(){
        return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    };

    std::unique_ptr<swss::Table> dash_ha_set_result_table_;
    std::unique_ptr<swss::Table> dash_ha_scope_result_table_;

    std::shared_ptr<swss::DBConnector> m_counter_db;
    std::unique_ptr<swss::Table> m_ha_set_name_table;

    std::unique_ptr<swss::DBConnector> m_dpuStateDbConnector;
    std::unique_ptr<swss::Table> m_dpuStateDbHaSetTable;
    std::unique_ptr<swss::Table> m_dpuStateDbHaScopeTable;

    swss::NotificationConsumer* m_haSetNotificationConsumer;
    swss::NotificationConsumer* m_haScopeNotificationConsumer;

private:
    DashHaCounter HaSetCounter;
    
public:
    const HaSetTable& getHaSetEntries() const { return m_ha_set_entries; };
    const HaScopeTable& getHaScopeEntries() const { return m_ha_scope_entries; };
    const DashBfdSessionTable& getBfdSessionPendingCreation() const { return m_bfd_session_pending_creation; };
    virtual HaScopeEntry getHaScopeForEni(const std::string& eni);
    void handleHaSetFCStatusUpdate(bool is_enabled) { HaSetCounter.handleStatusUpdate(is_enabled, m_ha_set_entries); }
};

#endif // DASHHAORCH_H
