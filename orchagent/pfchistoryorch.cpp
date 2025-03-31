#include "pfchistoryorch.h"
#include "portsorch.h"

#define DEFAULT_POLL_INTERVAL "1000"

extern sai_object_id_t gSwitchId;
extern sai_port_api_t *sai_port_api;
extern PortsOrch *gPortsOrch;

PfcHistoryOrch& PfcHistoryOrch::getInstance(DBConnector *db)
{
    SWSS_LOG_ENTER();
    static std::vector<string> tableNames = {
        CFG_PFC_STAT_HISTORY_TABLE_NAME
    };
    static const std::vector<sai_port_stat_t> portStatIds =
    {
        SAI_PORT_STAT_PFC_0_RX_PKTS,
        SAI_PORT_STAT_PFC_1_RX_PKTS,
        SAI_PORT_STAT_PFC_2_RX_PKTS,
        SAI_PORT_STAT_PFC_3_RX_PKTS,
        SAI_PORT_STAT_PFC_4_RX_PKTS,
        SAI_PORT_STAT_PFC_5_RX_PKTS,
        SAI_PORT_STAT_PFC_6_RX_PKTS,
        SAI_PORT_STAT_PFC_7_RX_PKTS,
    };
    static PfcHistoryOrch *historyOrch = new PfcHistoryOrch(db, tableNames, portStatIds);

    return *historyOrch;
}

PfcHistoryOrch::PfcHistoryOrch(
    DBConnector *db,
    std::vector<string> &tableNames,
    const vector<sai_port_stat_t> &portStatIds
):
    Orch(db, tableNames),
    c_portStatIds(portStatIds),
    m_countersDb(new DBConnector("COUNTERS_DB", 0))
{
    SWSS_LOG_ENTER();

    // clear history stats for safe restart
    removeAllPfcStatHistoryCounters();

    // if config_db has not yet been populated, provide the defaults so it is consistent with flex_counter_db
    swss::Table flexCounterTable(db, CFG_FLEX_COUNTER_TABLE_NAME);
    std::string poll_interval;
    if(!flexCounterTable.hget(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP, POLL_INTERVAL_FIELD, poll_interval)){
        flexCounterTable.hset(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP, POLL_INTERVAL_FIELD, DEFAULT_POLL_INTERVAL);
    }
    std::string status;
    if(!flexCounterTable.hget(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP, FLEX_COUNTER_STATUS_FIELD, status)){
        flexCounterTable.hset(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP, FLEX_COUNTER_STATUS_FIELD, "disable");
    }

    string pluginName = "pfc_stat_history.lua";
    string sha;
    try
    {
        string script = swss::loadLuaScript(pluginName);
        sha = swss::loadRedisScript(m_countersDb.get(), script);
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_ERROR("PFC STAT HISTORY flex counter group not set successfully: %s", e.what());
    }

    setFlexCounterGroupParameter(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP,
                                 DEFAULT_POLL_INTERVAL, // this will be overwritten by the config_db entry regardless
                                 STATS_MODE_READ, // don't clear counters on read
                                 PORT_PLUGIN_FIELD, // "context name" in syncd, "plugin name" here
                                 sha);
}

PfcHistoryOrch::~PfcHistoryOrch(void)
{
    SWSS_LOG_ENTER();
}

// Remove all PFC Historical Stats from COUNTERS_DB
void PfcHistoryOrch::removeAllPfcStatHistoryCounters()
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    const auto pfcHistTable = std::make_unique<Table>(m_countersDb.get(), COUNTERS_PFC_STAT_HISTORY_TABLE);
    pfcHistTable->getKeys(keys);
    for (auto key : keys)
    {
        pfcHistTable->del(key);
    }
}

void PfcHistoryOrch::doTask(Consumer& consumer){
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if ((consumer.getDbName() == "CONFIG_DB")
         && consumer.getTableName() == CFG_PFC_STAT_HISTORY_TABLE_NAME
    )
    {
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple t = it->second;
            string key = kfvKey(t);
            string op = kfvOp(t);
            std::vector<FieldValueTuple> fvt = kfvFieldsValues(t);

            Port port;
            if (!gPortsOrch->getPort(key, port))
            {
                return;
            }

            if (op == SET_COMMAND)
            {
                if (port.m_type != Port::PHY)
                {
                    SWSS_LOG_ERROR("%s not a physical port!", sai_serialize_object_id(port.m_port_id).c_str());
                    continue;
                }

                if (!c_portStatIds.empty())
                {
                    string key = string(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP) + ":" + sai_serialize_object_id(port.m_port_id);
                    string str = counterIdsToStr(c_portStatIds);

                    // FLEX_COUNTER_TABLE:PFC_STAT_HISTORY:oid:<port> : {PORT_COUNTER_ID_LIST: SAI_PORT_PFC_X_RX_PKTS}
                    startFlexCounterPolling(gSwitchId, key, str, PORT_COUNTER_ID_LIST);
                }
                else{
                    SWSS_LOG_ERROR("No port stat ids provided, polling on %s not started", sai_serialize_object_id(port.m_port_id).c_str());
                }
            }
            else if (op == DEL_COMMAND)
            {
                string key = string(PFC_STAT_HISTORY_FLEX_COUNTER_GROUP) + ":" + sai_serialize_object_id(port.m_port_id);
                // Unregister in syncd
                stopFlexCounterPolling(gSwitchId, key);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            }
            consumer.m_toSync.erase(it++);
        }
    }
}

string PfcHistoryOrch::counterIdsToStr(const vector<sai_port_stat_t> ids)
{
    SWSS_LOG_ENTER();

    string str;

    for (const auto& i: ids)
    {
        str += sai_serialize_port_stat(i) + ",";
    }

    // Remove trailing ','
    if (!str.empty())
    {
        str.pop_back();
    }

    return str;
}