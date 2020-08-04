#include <unordered_map>
#include "flexcounterorch.h"
#include "portsorch.h"
#include "select.h"
#include "notifier.h"
#include "redisclient.h"
#include "sai_serialize.h"
#include "pfcwdorch.h"
#include "bufferorch.h"
#include "flexcounterorch.h"
#include "debugcounterorch.h"

extern sai_port_api_t *sai_port_api;

extern PortsOrch *gPortsOrch;
extern IntfsOrch *gIntfsOrch;
extern BufferOrch *gBufferOrch;

#define BUFFER_POOL_WATERMARK_KEY   "BUFFER_POOL_WATERMARK"

unordered_map<string, string> flexCounterGroupMap =
{
    {"PORT", PORT_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PORT_BUFFER_DROP", PORT_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"QUEUE", QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PFCWD", PFC_WD_FLEX_COUNTER_GROUP},
    {"QUEUE_WATERMARK", QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PG_WATERMARK", PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {BUFFER_POOL_WATERMARK_KEY, BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"RIF", RIF_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"DEBUG_COUNTER", DEBUG_COUNTER_FLEX_COUNTER_GROUP},
};


FlexCounterOrch::FlexCounterOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_flexCounterDb(new DBConnector("FLEX_COUNTER_DB", 0)),
    m_flexCounterGroupTable(new ProducerTable(m_flexCounterDb.get(), FLEX_COUNTER_GROUP_TABLE))
{
    SWSS_LOG_ENTER();
}

FlexCounterOrch::~FlexCounterOrch(void)
{
    SWSS_LOG_ENTER();
}

void FlexCounterOrch::doTask(Consumer &consumer)
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

        string key =  kfvKey(t);
        string op = kfvOp(t);
        auto data = kfvFieldsValues(t);

        if (!flexCounterGroupMap.count(key))
        {
            SWSS_LOG_NOTICE("Invalid flex counter group input, %s", key.c_str());
            consumer.m_toSync.erase(it++);
            continue;
        }

        if (op == SET_COMMAND)
        {
            for (auto valuePair:data)
            {
                const auto &field = fvField(valuePair);
                const auto &value = fvValue(valuePair);

                if (field == POLL_INTERVAL_FIELD)
                {
                    vector<FieldValueTuple> fieldValues;
                    fieldValues.emplace_back(POLL_INTERVAL_FIELD, value);
                    m_flexCounterGroupTable->set(flexCounterGroupMap[key], fieldValues);
                }
                else if(field == FLEX_COUNTER_STATUS_FIELD)
                {
                    // Currently, the counters are disabled for polling by default
                    // The queue maps will be generated as soon as counters are enabled for polling
                    // Counter polling is enabled by pushing the COUNTER_ID_LIST/ATTR_ID_LIST, which contains
                    // the list of SAI stats/attributes of polling interest, to the FLEX_COUNTER_DB under the
                    // additional condition that the polling interval at that time is set nonzero positive,
                    // which is automatically satisfied upon the creation of the orch object that requires
                    // the syncd flex counter polling service
                    // This postponement is introduced by design to accelerate the initialization process
                    //
                    // generateQueueMap() is called as long as a field "FLEX_COUNTER_STATUS" event is heard,
                    // regardless of whether the key is "QUEUE" or whether the value is "enable" or "disable"
                    // This can be because generateQueueMap() installs a fundamental list of queue stats
                    // that need to be polled. So my doubt here is if queue watermark stats shall be piggybacked
                    // into the same function as they may not be counted as fundamental
                    gPortsOrch->generateQueueMap();
                    gPortsOrch->generatePriorityGroupMap();
                    gIntfsOrch->generateInterfaceMap();
                    // Install COUNTER_ID_LIST/ATTR_ID_LIST only when hearing buffer pool watermark enable event
                    if ((key == BUFFER_POOL_WATERMARK_KEY) && (value == "enable"))
                    {
                        gBufferOrch->generateBufferPoolWatermarkCounterIdList();
                    }

                    vector<FieldValueTuple> fieldValues;
                    fieldValues.emplace_back(FLEX_COUNTER_STATUS_FIELD, value);
                    m_flexCounterGroupTable->set(flexCounterGroupMap[key], fieldValues);
                }
                else
                {
                    SWSS_LOG_NOTICE("Unsupported field %s", field.c_str());
                }
            }
        }

        consumer.m_toSync.erase(it++);
    }
}
