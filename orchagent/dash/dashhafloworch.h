#pragma once

#include <map>
#include <memory>
#include <string>
#include <chrono>
#include <vector>

#include "dbconnector.h"
#include "zmqorch.h"
#include "zmqserver.h"
#include "saitypes.h"
#include "notifier.h"
#include "directory.h"
#include "sai_serialize.h"
#include "notifications.h"
#include "timer.h"
#include "orch.h"

extern sai_dash_flow_api_t* sai_dash_flow_api;
extern sai_object_id_t gSwitchId;

class DashHaFlowOrch;

class FlowDumpFilterManager
{
    friend class DashHaFlowOrch;

    struct FlowDumpFilterEntry
    {
        sai_object_id_t filter_id;
        std::string key;
        std::string op;
        std::string value;
    };

public:
    FlowDumpFilterManager() = default;
    ~FlowDumpFilterManager() = default;

    std::vector<sai_object_id_t> getFilterIds(const std::vector<std::string> &required_filter_keys) const;

protected:
    task_process_status addFilter(const std::string &key, const std::vector<swss::FieldValueTuple> &attrs);
    task_process_status removeFilter(const std::string &key);

private:
    sai_object_id_t createFilterSAI(const FlowDumpFilterEntry &filter);
    bool deleteFilterSAI(sai_object_id_t filter_id);

    std::map<std::string, FlowDumpFilterEntry> m_filter_cache;
};

class FlowApiHandler
{
public:
    FlowApiHandler(swss::DBConnector *dpu_state_db, swss::SelectableTimer *timer);
    virtual ~FlowApiHandler() = default;

    virtual bool initialize(const std::string &key, const std::vector<swss::FieldValueTuple> &attrs) = 0;
    virtual task_process_status handleSet(const std::string &table_name, const std::string &key, const std::vector<swss::FieldValueTuple> &attrs) = 0;
    virtual task_process_status handleDel(const std::string &table_name, const std::string &key) = 0;

    virtual void handleFinished() = 0;
    virtual void handleTimeout() = 0;
    virtual sai_object_id_t getSessionId() const { return m_session_id; }
    virtual std::string getKey() const { return m_key; }
    virtual bool isActive() const { return m_session_id != SAI_NULL_OBJECT_ID; }
    virtual swss::SelectableTimer* getTimer() const { return m_timer; }

protected:
    virtual task_process_status createSession() = 0;
    virtual sai_object_id_t createSessionSAI() = 0;
    virtual void reset() = 0;

    void deleteSession();
    bool deleteSessionSAI();
    void updateState(const std::string &state, const std::string &key, std::vector<swss::FieldValueTuple> fvs);

    std::string m_key;
    sai_object_id_t m_session_id;
    swss::SelectableTimer* m_timer;
    std::chrono::steady_clock::time_point m_creation_time;
    std::chrono::steady_clock::time_point m_last_state_time;
    std::shared_ptr<swss::Table> m_state_table;
};

class BulkSyncHandler : public FlowApiHandler
{
public:
    BulkSyncHandler(swss::DBConnector *dpu_state_db, swss::SelectableTimer *timer);
    virtual ~BulkSyncHandler();

    bool initialize(const std::string &key, const std::vector<swss::FieldValueTuple> &attrs) override;
    task_process_status handleSet(const std::string &table_name, const std::string &key, const std::vector<swss::FieldValueTuple> &attrs) override;
    task_process_status handleDel(const std::string &table_name, const std::string &key) override;
    void handleFinished() override;
    void handleTimeout() override;

protected:
    task_process_status createSession() override;
    sai_object_id_t createSessionSAI() override;
    void reset() override;

private:
    sai_object_id_t m_ha_set_id;
    std::string m_target_server_ip;
    uint16_t m_target_server_port;
    uint32_t m_timeout_sec;

    static constexpr uint32_t DEFAULT_TIMEOUT_SEC = 120;
};

class FlowDumpHandler : public FlowApiHandler
{
public:
    FlowDumpHandler(swss::DBConnector *dpu_state_db, swss::SelectableTimer *timer, std::shared_ptr<FlowDumpFilterManager> filter_manager);
    virtual ~FlowDumpHandler();

    bool initialize(const std::string &key, const std::vector<swss::FieldValueTuple> &attrs) override;
    task_process_status handleSet(const std::string &table_name, const std::string &key, const std::vector<swss::FieldValueTuple> &attrs) override;
    task_process_status handleDel(const std::string &table_name, const std::string &key) override;
    void handleFinished() override;
    void handleTimeout() override;

protected:
    task_process_status createSession() override;
    sai_object_id_t createSessionSAI() override;
    void reset() override;

    bool m_flow_state;
    std::vector<std::string> m_required_filter_keys;
    uint32_t m_max_flows;
    uint32_t m_timeout_sec;
    std::string m_output_file;

private:
    std::shared_ptr<FlowDumpFilterManager> m_filter_manager;

    static constexpr uint32_t DEFAULT_TIMEOUT_SEC = 300;
    static constexpr uint32_t MAX_FLOWS_DEFAULT = 1000;
};

class DashHaFlowOrch : public ZmqOrch
{
public:
    static constexpr const char* SESSION_TYPE_BULK_SYNC = "bulk_sync";
    static constexpr const char* SESSION_TYPE_FLOW_DUMP = "flow_dump";

    DashHaFlowOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer);

protected:
    std::map<std::string, std::shared_ptr<FlowApiHandler>> m_handlers;

    std::unique_ptr<swss::DBConnector> m_dpuStateDb;
    swss::SelectableTimer* m_sync_timer;
    swss::SelectableTimer* m_dump_timer;
    swss::ExecutableTimer* m_sync_executor;
    swss::ExecutableTimer* m_dump_executor;

    swss::NotificationConsumer* m_flowBulkGetSessionNotificationConsumer;
    std::shared_ptr<FlowDumpFilterManager> m_filter_manager;

    void doTask(ConsumerBase &consumer);
    void doTask(swss::NotificationConsumer &consumer);
    void doTask(swss::SelectableTimer &timer);
    void doTaskFlowSyncSessionTable(ConsumerBase &consumer);
    void doTaskFlowDumpFilterTable(ConsumerBase &consumer);
    
    std::string getTypeFromAttrs(const std::vector<swss::FieldValueTuple> &attrs);

    void handleSessionNotification(const std::string &notification_name, const std::string &data, const std::vector<swss::FieldValueTuple> &values);
    void handleSessionFinished(sai_object_id_t session_id);
    void handleTimerExpired(swss::SelectableTimer *timer);

    bool registerFlowBulkGetSessionNotifier();
};
