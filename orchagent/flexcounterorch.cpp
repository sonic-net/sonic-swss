#include <unordered_map>
#include "flexcounterorch.h"
#include "portsorch.h"
#include "switchorch.h"
#include "select.h"
#include "notifier.h"
#include "redisclient.h"
#include "sai_serialize.h"
#include "pfcwdorch.h"

extern sai_port_api_t *sai_port_api;

extern PortsOrch *gPortsOrch;

unordered_map<string, string> flexCounterGroupMap =
{
    {"PORT", PORT_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"QUEUE", QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PFCWD", PFC_WD_FLEX_COUNTER_GROUP},
    {"SENSORS", SWITCH_SENSORS_FLEX_GROUP},
};


FlexCounterOrch::FlexCounterOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_flexCounterDb(new DBConnector(FLEX_COUNTER_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
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

    if (!gPortsOrch->isInitDone())
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
                    // Currently the counters are disabled by default
                    // The queue maps will be generated as soon as counters are enabled
                    gPortsOrch->generateQueueMap();

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
