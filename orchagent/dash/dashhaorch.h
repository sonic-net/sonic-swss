#ifndef DASHHAORCH_H
#define DASHHAORCH_H
#include <map>

#include "dbconnector.h"
#include "dashorch.h"
#include "zmqorch.h"
#include "zmqserver.h"
#include "saitypes.h"
#include "notifier.h"
#include "directory.h"
#include "sai_serialize.h"
#include "notifications.h"

#include "dash_api/ha_set.pb.h"
#include "dash_api/ha_scope.pb.h"

#include "pbutils.h"

struct HaSetEntry
{
    sai_object_id_t ha_set_id;
    dash::ha_set::HaSet metadata;
};

struct HaScopeEntry
{
    sai_object_id_t ha_scope_id;
    dash::ha_scope::HaScope metadata;
    std::time_t last_role_start_time;
};

typedef std::map<std::string, HaSetEntry> HaSetTable;
typedef std::map<std::string, HaScopeEntry> HaScopeTable;

template <typename T>
bool in(T value, std::initializer_list<T> list) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

class DashHaOrch : public ZmqOrch
{
public:
    DashHaOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, DashOrch *dash_orch, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer);

protected:
    HaSetTable m_ha_set_entries;
    HaScopeTable m_ha_scope_entries;

    DashOrch *m_dash_orch;

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
    bool setEniHaScopeId(const sai_object_id_t eni_id, const sai_object_id_t ha_scope_id);
    bool register_ha_set_notifier();
    bool register_ha_scope_notifier();

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
    
    std::unique_ptr<swss::DBConnector> m_dpuStateDbConnector;
    std::unique_ptr<swss::Table> m_dpuStateDbHaSetTable;
    std::unique_ptr<swss::Table> m_dpuStateDbHaScopeTable;

    swss::NotificationConsumer* m_haSetNotificationConsumer;
    swss::NotificationConsumer* m_haScopeNotificationConsumer;

public:
    const HaSetTable& getHaSetEntries() const { return m_ha_set_entries; };
    const HaScopeTable& getHaScopeEntries() const { return m_ha_scope_entries; };
};

#endif // DASHHAORCH_H
