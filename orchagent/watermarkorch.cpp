#include "orchagent/watermarkorch.h"
#include "sai_serialize.h"
#include "orchagent/portsorch.h"
#include "orchagent/notifier.h"
#include "converter.h"
#include "orchagent/bufferorch.h"
#include <inttypes.h>

#define DEFAULT_TELEMETRY_INTERVAL 120

#define CLEAR_PG_HEADROOM_REQUEST "PG_HEADROOM"
#define CLEAR_PG_SHARED_REQUEST "PG_SHARED"
#define CLEAR_QUEUE_SHARED_UNI_REQUEST "Q_SHARED_UNI"
#define CLEAR_QUEUE_SHARED_MULTI_REQUEST "Q_SHARED_MULTI"
#define CLEAR_QUEUE_SHARED_ALL_REQUEST "Q_SHARED_ALL"
#define CLEAR_BUFFER_POOL_REQUEST "BUFFER_POOL"
#define CLEAR_HEADROOM_POOL_REQUEST "HEADROOM_POOL"

extern PortsOrch *gPortsOrch;
extern BufferOrch *gBufferOrch;


WatermarkOrch::WatermarkOrch(DBConnector *db, const vector<string> &tables):
    Orch(db, tables)
{
    SWSS_LOG_ENTER();

    m_countersDb = make_shared<DBConnector>("COUNTERS_DB", 0);
    m_appDb = make_shared<DBConnector>("APPL_DB", 0);
    m_countersTable = make_shared<Table>(m_countersDb.get(), COUNTERS_TABLE);
    m_periodicWatermarkTable = make_shared<Table>(m_countersDb.get(), PERIODIC_WATERMARKS_TABLE);
    m_persistentWatermarkTable = make_shared<Table>(m_countersDb.get(), PERSISTENT_WATERMARKS_TABLE);
    m_userWatermarkTable = make_shared<Table>(m_countersDb.get(), USER_WATERMARKS_TABLE);

    m_clearNotificationConsumer = new swss::NotificationConsumer(
            m_appDb.get(),
            "WATERMARK_CLEAR_REQUEST");
    auto clearNotifier = new Notifier(m_clearNotificationConsumer, this, "WM_CLEAR_NOTIFIER");
    Orch::addExecutor(clearNotifier);

    auto intervT = timespec { .tv_sec = DEFAULT_TELEMETRY_INTERVAL , .tv_nsec = 0 };
    m_telemetryTimer = new SelectableTimer(intervT);
    auto executorT = new ExecutableTimer(m_telemetryTimer, this, "WM_TELEMETRY_TIMER");
    Orch::addExecutor(executorT);
}

WatermarkOrch::~WatermarkOrch()
{
    SWSS_LOG_ENTER();
}

void WatermarkOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        std::vector<FieldValueTuple> fvt = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            if (consumer.getTableName() == CFG_WATERMARK_TABLE_NAME)
            {
                handleWmConfigUpdate(key, fvt);
            }
            else if (consumer.getTableName() == CFG_FLEX_COUNTER_TABLE_NAME)
            {
                handleFcConfigUpdate(key, fvt);
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_WARN("Unsupported op %s", op.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}

void WatermarkOrch::handleWmConfigUpdate(const std::string &key, const std::vector<FieldValueTuple> &fvt)
{
    SWSS_LOG_ENTER();
    if (key == "TELEMETRY_INTERVAL")
    {
        for (std::pair<std::basic_string<char>, std::basic_string<char> > i: fvt)
        {
            if (i.first == "interval")
            {
                auto intervT = timespec { .tv_sec = static_cast<time_t>(to_uint<uint32_t>(i.second.c_str())), .tv_nsec = 0 };
                m_telemetryTimer->setInterval(intervT);
                // reset the timer interval when current timer expires
                m_timerChanged = true;
            }
            else
            {
                SWSS_LOG_WARN("Unsupported key: %s", i.first.c_str());
            }
        }
    }
}

void WatermarkOrch::handleFcConfigUpdate(const std::string &key, const std::vector<FieldValueTuple> &fvt)
{
    SWSS_LOG_ENTER();
    uint8_t prevStatus = m_wmStatus;
    if (key == "QUEUE_WATERMARK" || key == "PG_WATERMARK")
    {
        for (std::pair<std::basic_string<char>, std::basic_string<char> > i: fvt)
        {
            if (i.first == "FLEX_COUNTER_STATUS")
            {
                if (i.second == "enable")
                {
                    m_wmStatus = (uint8_t) (m_wmStatus | groupToMask.at(key));
                }
                else if (i.second == "disable")
                {
                    m_wmStatus = (uint8_t) (m_wmStatus & ~(groupToMask.at(key)));
                }
            }
        }
        if (!prevStatus && m_wmStatus)
        {
            m_telemetryTimer->start();
        }
    SWSS_LOG_DEBUG("Status of WMs: %u", m_wmStatus);
    }
}

void WatermarkOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();
    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if (m_pg_ids.empty())
    {
        init_pg_ids();
    }

    if (m_multicast_queue_ids.empty() and m_unicast_queue_ids.empty() and m_all_queue_ids.empty())
    {
        init_queue_ids();
    }

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    Table * table = NULL;

    if (op == "PERSISTENT")
    {
        table = m_persistentWatermarkTable.get();
    }
    else if (op == "USER")
    {
        table = m_userWatermarkTable.get();
    }
    else
    {
        SWSS_LOG_WARN("Unknown watermark clear request op: %s", op.c_str());
        return;
    }

    if (data == CLEAR_PG_HEADROOM_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES",
                      m_pg_ids);
    }
    else if (data == CLEAR_PG_SHARED_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES",
                      m_pg_ids);
    }
    else if (data == CLEAR_QUEUE_SHARED_UNI_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
                      m_unicast_queue_ids);
    }
    else if (data == CLEAR_QUEUE_SHARED_MULTI_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
                      m_multicast_queue_ids);
    }
    else if (data == CLEAR_QUEUE_SHARED_ALL_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
                      m_all_queue_ids);
    }
    else if (data == CLEAR_BUFFER_POOL_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES",
                      gBufferOrch->getBufferPoolNameOidMap());
    }
    else if (data == CLEAR_HEADROOM_POOL_REQUEST)
    {
        clearSingleWm(table,
                      "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES",
                      gBufferOrch->getBufferPoolNameOidMap());
    }
    else
    {
        SWSS_LOG_WARN("Unknown watermark clear request data: %s", data.c_str());
        return;
    }
}

void WatermarkOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (m_pg_ids.empty())
    {
        init_pg_ids();
    }

    if (m_multicast_queue_ids.empty() and m_unicast_queue_ids.empty() and m_all_queue_ids.empty())
    {
        init_queue_ids();
    }

    if (&timer == m_telemetryTimer)
    {
        if (m_timerChanged)
        {
            m_telemetryTimer->reset();
            m_timerChanged = false;
        }
        if (!m_wmStatus)
        {
            m_telemetryTimer->stop();
        }

        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES",
                      m_pg_ids);
        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES",
                      m_pg_ids);
        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
                      m_unicast_queue_ids);
        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
                      m_multicast_queue_ids);
        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
                      m_all_queue_ids);
        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES",
                      gBufferOrch->getBufferPoolNameOidMap());
        clearSingleWm(m_periodicWatermarkTable.get(),
                      "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES",
                      gBufferOrch->getBufferPoolNameOidMap());
        SWSS_LOG_DEBUG("Periodic watermark cleared by timer!");
    }
}

void WatermarkOrch::init_pg_ids()
{
    SWSS_LOG_ENTER();
    std::vector<FieldValueTuple> values;
    Table pg_index_table(m_countersDb.get(), COUNTERS_PG_INDEX_MAP);
    pg_index_table.get("", values);
    for (auto fv: values)
    {
        sai_object_id_t id;
        sai_deserialize_object_id(fv.first, id);
        m_pg_ids.push_back(id);
    }
}

void WatermarkOrch::init_queue_ids()
{
    SWSS_LOG_ENTER();
    std::vector<FieldValueTuple> values;
    Table m_queue_type_table(m_countersDb.get(), COUNTERS_QUEUE_TYPE_MAP);
    m_queue_type_table.get("", values);
    for (auto fv: values)
    {
        sai_object_id_t id;
        sai_deserialize_object_id(fv.first, id);
        if (fv.second == "SAI_QUEUE_TYPE_UNICAST")
        {
            m_unicast_queue_ids.push_back(id);
        }
        else if (fv.second == "SAI_QUEUE_TYPE_MULTICAST")
        {
            m_multicast_queue_ids.push_back(id);
        }
        else if (fv.second == "SAI_QUEUE_TYPE_ALL")
        {
            m_all_queue_ids.push_back(id);
        }
    }
}

void WatermarkOrch::clearSingleWm(Table *table, string wm_name, vector<sai_object_id_t> &obj_ids)
{
    /* Zero-out some WM in some table for some vector of object ids*/
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("clear WM %s, for %zu obj ids", wm_name.c_str(), obj_ids.size());

    vector<FieldValueTuple> vfvt = {{wm_name, "0"}};

    for (sai_object_id_t id: obj_ids)
    {
        table->set(sai_serialize_object_id(id), vfvt);
    }
}

void WatermarkOrch::clearSingleWm(Table *table, string wm_name, const object_reference_map &nameOidMap)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_DEBUG("clear WM %s, for %zu obj ids", wm_name.c_str(), nameOidMap.size());

    vector<FieldValueTuple> fvTuples = {{wm_name, "0"}};

    for (const auto &it : nameOidMap)
    {
        table->set(sai_serialize_object_id(it.second.m_saiObjectId), fvTuples);
    }
}
