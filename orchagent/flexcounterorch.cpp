#include <unordered_map>
#include "portsorch.h"
#include "fabricportsorch.h"
#include "select.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "pfcwdorch.h"
#include "bufferorch.h"
#include "flexcounterorch.h"
#include "debugcounterorch.h"
#include "directory.h"
#include "copporch.h"

extern sai_port_api_t *sai_port_api;

extern PortsOrch *gPortsOrch;
extern FabricPortsOrch *gFabricPortsOrch;
extern IntfsOrch *gIntfsOrch;
extern BufferOrch *gBufferOrch;
extern Directory<Orch*> gDirectory;
extern CoppOrch *gCoppOrch;

#define BUFFER_POOL_WATERMARK_KEY   "BUFFER_POOL_WATERMARK"
#define PORT_KEY                    "PORT"
#define PORT_BUFFER_DROP_KEY        "PORT_BUFFER_DROP"
#define QUEUE_KEY                   "QUEUE"
#define PG_WATERMARK_KEY            "PG_WATERMARK"
#define RIF_KEY                     "RIF"
#define ACL_KEY                     "ACL"
#define TUNNEL_KEY                  "TUNNEL"
#define FLOW_CNT_TRAP_KEY           "FLOW_CNT_TRAP"

unordered_map<string, string> flexCounterGroupMap =
{
    {"PORT", PORT_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PORT_RATES", PORT_RATE_COUNTER_FLEX_COUNTER_GROUP},
    {"PORT_BUFFER_DROP", PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP},
    {"QUEUE", QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PFCWD", PFC_WD_FLEX_COUNTER_GROUP},
    {"QUEUE_WATERMARK", QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PG_WATERMARK", PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"PG_DROP", PG_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {BUFFER_POOL_WATERMARK_KEY, BUFFER_POOL_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"RIF", RIF_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {"RIF_RATES", RIF_RATE_COUNTER_FLEX_COUNTER_GROUP},
    {"DEBUG_COUNTER", DEBUG_COUNTER_FLEX_COUNTER_GROUP},
    {"ACL", ACL_COUNTER_FLEX_COUNTER_GROUP},
    {"TUNNEL", TUNNEL_STAT_COUNTER_FLEX_COUNTER_GROUP},
    {FLOW_CNT_TRAP_KEY, HOSTIF_TRAP_COUNTER_FLEX_COUNTER_GROUP},
};


FlexCounterOrch::FlexCounterOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_flexCounterConfigTable(db, CFG_FLEX_COUNTER_TABLE_NAME),
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

    VxlanTunnelOrch* vxlan_tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    if (gPortsOrch && !gPortsOrch->allPortsReady())
    {
        return;
    }

    if (gFabricPortsOrch && !gFabricPortsOrch->allPortsReady())
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
            auto itDelay = std::find(std::begin(data), std::end(data), FieldValueTuple(FLEX_COUNTER_DELAY_STATUS_FIELD, "true"));

            if (itDelay != data.end())
            {
                consumer.m_toSync.erase(it++);
                continue;
            }
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
                    if(gPortsOrch && (value == "enable"))
                    {
                        if(key == PORT_KEY)
                        {
                            gPortsOrch->generatePortCounterMap();
                            m_port_counter_enabled = true;
                        }
                        else if(key == PORT_BUFFER_DROP_KEY)
                        {
                            gPortsOrch->generatePortBufferDropCounterMap();
                            m_port_buffer_drop_counter_enabled = true;
                        }
                        else if(key == QUEUE_KEY)
                        {
                            gPortsOrch->generateQueueMap();
                        }
                        else if(key == PG_WATERMARK_KEY)
                        {
                            gPortsOrch->generatePriorityGroupMap();
                        }
                    }
                    if(gIntfsOrch && (key == RIF_KEY) && (value == "enable"))
                    {
                        gIntfsOrch->generateInterfaceMap();
                    }
                    if (gBufferOrch && (key == BUFFER_POOL_WATERMARK_KEY) && (value == "enable"))
                    {
                        gBufferOrch->generateBufferPoolWatermarkCounterIdList();
                    }
                    if (gFabricPortsOrch)
                    {
                        gFabricPortsOrch->generateQueueStats();
                    }
                    if (vxlan_tunnel_orch && (key== TUNNEL_KEY) && (value == "enable"))
                    {
                        vxlan_tunnel_orch->generateTunnelCounterMap();
                    }
                    if (gCoppOrch && (key == FLOW_CNT_TRAP_KEY))
                    {
                        if (value == "enable")
                        {
                            m_hostif_trap_counter_enabled = true;
                            gCoppOrch->generateHostIfTrapCounterIdList();
                        }
                        else if (value == "disable")
                        {
                            gCoppOrch->clearHostIfTrapCounterIdList();
                            m_hostif_trap_counter_enabled = false;
                        }
                    }
                    vector<FieldValueTuple> fieldValues;
                    fieldValues.emplace_back(FLEX_COUNTER_STATUS_FIELD, value);
                    m_flexCounterGroupTable->set(flexCounterGroupMap[key], fieldValues);
                }
                else if(field == FLEX_COUNTER_DELAY_STATUS_FIELD)
                {
                    // This field is ignored since it is being used before getting into this loop.
                    // If it is exist and the value is 'true' we need to skip the iteration in order to delay the counter creation.
                    // The field will clear out and counter will be created when enable_counters script is called.
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

bool FlexCounterOrch::getPortCountersState() const
{
    return m_port_counter_enabled;
}

bool FlexCounterOrch::getPortBufferDropCountersState() const
{
    return m_port_buffer_drop_counter_enabled;
}

bool FlexCounterOrch::bake()
{
    /*
     * bake is called during warmreboot reconciling procedure.
     * By default, it should fetch items from the tables the sub agents listen to,
     * and then push them into m_toSync of each sub agent.
     * The motivation is to make sub agents handle the saved entries first and then handle the upcoming entries.
     */

    std::deque<KeyOpFieldsValuesTuple> entries;
    vector<string> keys;
    m_flexCounterConfigTable.getKeys(keys);
    for (const auto &key: keys)
    {
        if (!flexCounterGroupMap.count(key))
        {
            SWSS_LOG_NOTICE("FlexCounterOrch: Invalid flex counter group intput %s is skipped during reconciling", key.c_str());
            continue;
        }

        if (key == BUFFER_POOL_WATERMARK_KEY)
        {
            SWSS_LOG_NOTICE("FlexCounterOrch: Do not handle any FLEX_COUNTER table for %s update during reconciling",
                            BUFFER_POOL_WATERMARK_KEY);
            continue;
        }

        KeyOpFieldsValuesTuple kco;

        kfvKey(kco) = key;
        kfvOp(kco) = SET_COMMAND;

        if (!m_flexCounterConfigTable.get(key, kfvFieldsValues(kco)))
        {
            continue;
        }
        entries.push_back(kco);
    }
    Consumer* consumer = dynamic_cast<Consumer *>(getExecutor(CFG_FLEX_COUNTER_TABLE_NAME));
    return consumer->addToSync(entries);
}
