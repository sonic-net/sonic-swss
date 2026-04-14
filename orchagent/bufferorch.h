#ifndef SWSS_BUFFORCH_H
#define SWSS_BUFFORCH_H

#include <string>
#include <map>
#include <unordered_map>
#include "orch.h"
#include "portsorch.h"
#include "redisapi.h"
#include "saiattr.h"

#include "buffer/bufferhelper.h"
#include "high_frequency_telemetry/counternameupdater.h"

#define BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP "BUFFER_POOL_WATERMARK_STAT_COUNTER"
#define BUFFER_POOL_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS  "60000"

const string buffer_size_field_name         = "size";
const string buffer_pool_type_field_name    = "type";
const string buffer_pool_mode_field_name    = "mode";
const string buffer_pool_field_name         = "pool";
const string buffer_pool_mode_dynamic_value = "dynamic";
const string buffer_pool_mode_static_value  = "static";
const string buffer_pool_xoff_field_name    = "xoff";
const string buffer_xon_field_name          = "xon";
const string buffer_xon_offset_field_name   = "xon_offset";
const string buffer_xoff_field_name         = "xoff";
const string buffer_dynamic_th_field_name   = "dynamic_th";
const string buffer_static_th_field_name    = "static_th";
const string buffer_profile_field_name      = "profile";
const string buffer_value_ingress           = "ingress";
const string buffer_value_egress            = "egress";
const string buffer_value_both              = "both";
const string buffer_profile_list_field_name = "profile_list";
const string buffer_headroom_type_field_name= "headroom_type";

struct PortBufferProfileListTask
{
    struct PortContext
    {
        std::string port_name;
        sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;
        SaiAttrWrapper attr = {};
        sai_status_t status = SAI_STATUS_NOT_EXECUTED;
    };

    KeyOpFieldsValuesTuple kofvs;
    std::vector<PortContext> ports;
};

struct PriorityGroupTask
{
    struct PgContext
    {
        size_t index;
        bool update_sai = true;
        bool counter_was_added = false;
        bool counter_needs_to_add = false;
        sai_object_id_t pg_id = SAI_NULL_OBJECT_ID;
        SaiAttrWrapper attr = {};
        sai_status_t status = SAI_STATUS_NOT_EXECUTED;
    };

    struct PortContext
    {
        std::string port_name;
        std::vector<PgContext> pgs;
    };

    KeyOpFieldsValuesTuple kofvs;
    std::vector<PortContext> ports;
};

struct QueueTask
{
    struct QueueContext
    {
        size_t index;
        bool update_sai = true;
        bool counter_was_added = false;
        bool counter_needs_to_add = false;
        sai_object_id_t queue_id = SAI_NULL_OBJECT_ID;
        SaiAttrWrapper attr = {};
        sai_status_t status = SAI_STATUS_NOT_EXECUTED;
    };

    struct PortContext
    {
        std::string port_name;
        bool local_port = false;
        std::string local_port_name;
        std::vector<QueueContext> queues;
    };

    KeyOpFieldsValuesTuple kofvs;
    std::vector<PortContext> ports;
};

class BufferOrch : public Orch
{
public:
    BufferOrch(DBConnector *applDb, DBConnector *confDb, DBConnector *stateDb, vector<string> &tableNames);
    bool isPortReady(const std::string& port_name) const;
    static type_map m_buffer_type_maps;
    void generateBufferPoolWatermarkCounterIdList(void);
    const object_reference_map &getBufferPoolNameOidMap(void);
    void getBufferObjectsWithNonZeroProfile(vector<string> &nonZeroQueues, const string &table);

private:
    typedef task_process_status (BufferOrch::*buffer_table_handler)(KeyOpFieldsValuesTuple &tuple);
    typedef map<string, buffer_table_handler> buffer_table_handler_map;
    typedef pair<string, buffer_table_handler> buffer_handler_pair;

    typedef void (BufferOrch::*buffer_table_flush_handler)(Consumer& consumer);
    typedef map<string, buffer_table_flush_handler> buffer_table_flush_handler_map;
    typedef pair<string, buffer_table_flush_handler> buffer_flush_handler_pair;

    void doTask() override;
    virtual void doTask(Consumer& consumer);
    void clearBufferPoolWatermarkCounterIdList(const sai_object_id_t object_id);
    void initTableHandlers();
    void initBufferReadyLists(DBConnector *confDb, DBConnector *applDb);
    void initBufferReadyList(Table& table, bool isConfigDb);
    void initVoqBufferReadyList(Table& table, bool isConfigDb);
    void initFlexCounterGroupTable(void);
    void initBufferConstants();
    task_process_status processBufferPool(KeyOpFieldsValuesTuple &tuple);
    task_process_status processBufferProfile(KeyOpFieldsValuesTuple &tuple);

    // These methods process input task and add operations to the bulk buffer. This is first stage.
    task_process_status processQueue(KeyOpFieldsValuesTuple &tuple);
    task_process_status processPriorityGroup(KeyOpFieldsValuesTuple &tuple);
    task_process_status processIngressBufferProfileList(KeyOpFieldsValuesTuple &tuple);
    task_process_status processEgressBufferProfileList(KeyOpFieldsValuesTuple &tuple);

    // These methods flush the bulk buffer and update SAI return status codes per task.
    void processQueueBulk(Consumer& consumer);
    void processPriorityGroupBulk(Consumer& consumer);
    void processIngressBufferProfileListBulk(Consumer& consumer);
    void processEgressBufferProfileListBulk(Consumer& consumer);

    // These methods are invoked by the corresponding *Bulk methods after SAI operations complete.
    // These handle SAI return status code per task. This is second stage.
    task_process_status processQueuePost(const QueueTask& task);
    task_process_status processPriorityGroupPost(const PriorityGroupTask& task);
    task_process_status processIngressBufferProfileListPost(const PortBufferProfileListTask& task);
    task_process_status processEgressBufferProfileListPost(const PortBufferProfileListTask& task);

    buffer_table_handler_map m_bufferHandlerMap;
    buffer_table_flush_handler_map m_bufferFlushHandlerMap;
    std::unordered_map<std::string, bool> m_ready_list;
    std::unordered_map<std::string, std::vector<std::string>> m_port_ready_list_ref;

    Table m_stateBufferMaximumValueTable;

    unique_ptr<CounterNameMapUpdater> m_counterNameMapUpdater;
    unique_ptr<DBConnector> m_countersDb;

    bool m_isBufferPoolWatermarkCounterIdListGenerated = false;
    set<string> m_partiallyAppliedQueues;

    // Bulk task buffers per DB operation
    std::map<std::string, std::vector<PortBufferProfileListTask>> m_portIngressBufferProfileListBulk;
    std::map<std::string, std::vector<PortBufferProfileListTask>> m_portEgressBufferProfileListBulk;
    std::map<std::string, std::vector<PriorityGroupTask>> m_priorityGroupBulk;
    std::map<std::string, std::vector<QueueTask>> m_queueBulk;

    // Buffer OA helper
    BufferHelper m_bufHlpr;
};
#endif /* SWSS_BUFFORCH_H */

