#ifndef __QOSMGR__
#define __QOSMGR__

#include "dbconnector.h"
#include "orch.h"

#include <map>
#include <string>
#include <vector>

/*
 * STATE_DB table-name constants for the QoS objects this daemon manages.
 * CFG_* table-name constants (DSCP_TO_TC_MAP, ...) come from swss-common's
 * cfg_schema.h via "schema.h". STATE_* QoS tables have no upstream constants,
 * so they are defined here (same pattern as mirrormgr.h / nvgretunnelmgr.h).
 */
#ifndef STATE_DSCP_TO_TC_MAP_TABLE_NAME
#define STATE_DSCP_TO_TC_MAP_TABLE_NAME      "DSCP_TO_TC_MAP_TABLE"
#endif
#ifndef STATE_DOT1P_TO_TC_MAP_TABLE_NAME
#define STATE_DOT1P_TO_TC_MAP_TABLE_NAME     "DOT1P_TO_TC_MAP_TABLE"
#endif
#ifndef STATE_TC_TO_QUEUE_MAP_TABLE_NAME
#define STATE_TC_TO_QUEUE_MAP_TABLE_NAME     "TC_TO_QUEUE_MAP_TABLE"
#endif
#ifndef STATE_TC_TO_DSCP_MAP_TABLE_NAME
#define STATE_TC_TO_DSCP_MAP_TABLE_NAME      "TC_TO_DSCP_MAP_TABLE"
#endif
#ifndef STATE_SCHEDULER_TABLE_NAME
#define STATE_SCHEDULER_TABLE_NAME           "SCHEDULER_TABLE"
#endif
#ifndef STATE_WRED_PROFILE_TABLE_NAME
#define STATE_WRED_PROFILE_TABLE_NAME        "WRED_PROFILE_TABLE"
#endif
#ifndef STATE_QUEUE_TABLE_NAME
#define STATE_QUEUE_TABLE_NAME               "QUEUE_TABLE"
#endif
#ifndef STATE_PORT_QOS_MAP_TABLE_NAME
#define STATE_PORT_QOS_MAP_TABLE_NAME        "PORT_QOS_MAP_TABLE"
#endif

/*
 * Field names, matching orchagent/qosorch.h exactly so the same CONFIG_DB
 * data drives both the SAI path and this switchdev kernel path.
 */
#define QOS_FIELD_DSCP_TO_TC                 "dscp_to_tc_map"
#define QOS_FIELD_DOT1P_TO_TC                "dot1p_to_tc_map"
#define QOS_FIELD_TC_TO_QUEUE                "tc_to_queue_map"
#define QOS_FIELD_TC_TO_DSCP                 "tc_to_dscp_map"
#define QOS_FIELD_SCHEDULER                  "scheduler"
#define QOS_FIELD_WRED_PROFILE               "wred_profile"

/* WRED_PROFILE fields */
#define WRED_FIELD_RED_MIN_THRESHOLD         "red_min_threshold"
#define WRED_FIELD_RED_MAX_THRESHOLD         "red_max_threshold"
#define WRED_FIELD_RED_DROP_PROBABILITY      "red_drop_probability"
#define WRED_FIELD_GREEN_MIN_THRESHOLD       "green_min_threshold"
#define WRED_FIELD_GREEN_MAX_THRESHOLD       "green_max_threshold"
#define WRED_FIELD_YELLOW_MIN_THRESHOLD      "yellow_min_threshold"
#define WRED_FIELD_YELLOW_MAX_THRESHOLD      "yellow_max_threshold"
#define WRED_FIELD_GREEN_DROP_PROBABILITY    "green_drop_probability"
#define WRED_FIELD_YELLOW_DROP_PROBABILITY   "yellow_drop_probability"
#define WRED_FIELD_ECN                       "ecn"

/* SCHEDULER fields */
#define SCHED_FIELD_TYPE                     "type"
#define SCHED_FIELD_WEIGHT                   "weight"
#define SCHED_FIELD_METER_TYPE               "meter_type"
#define SCHED_FIELD_CIR                      "cir"
#define SCHED_FIELD_CBS                      "cbs"
#define SCHED_FIELD_PIR                      "pir"
#define SCHED_FIELD_PBS                      "pbs"

#define SCHED_TYPE_DWRR                      "DWRR"
#define SCHED_TYPE_WRR                       "WRR"
#define SCHED_TYPE_STRICT                    "STRICT"

/* Value limits (match qosorch: DSCP_MAX_VAL=63, EXP_MAX_VAL=7, 8 TCs, 8 queues) */
#define QOS_DSCP_MAX                         63
#define QOS_PRIO_MAX                         7
#define QOS_TC_MAX                           7
#define QOS_QUEUE_MAX                        7

#define PORT_NAME_GLOBAL                     "global"

namespace swss {

/*
 * QosMgr — switchdev QoS manager (replaces QosOrch).
 *
 * Reads the QoS CONFIG_DB tables and programs the kernel directly via tc:
 *   - DSCP_TO_TC_MAP / DOT1P_TO_TC_MAP  -> tc filter ... skbedit priority (classification)
 *   - TC_TO_QUEUE_MAP                   -> mqprio (tc -> TX queue)
 *   - TC_TO_DSCP_MAP                    -> tc pedit (egress DSCP rewrite)
 *   - SCHEDULER                         -> drr/prio/tbf per-queue qdisc
 *   - WRED_PROFILE                      -> red qdisc (single-color + ECN)
 *   - PORT_QOS_MAP                      -> binds maps to a port
 *   - QUEUE                             -> binds scheduler+WRED to a port's queue
 *
 * Color-aware WRED (green/yellow) and the ASIC-internal maps (TC_TO_PG,
 * PFC_*_TO_PG, *_TO_FC, MPLS_TC_TO_TC) have no kernel equivalent and are
 * logged-ignored / marked unsupported. Status is written to STATE_DB; there is
 * no APP_DB or orchagent hand-off — the kernel is the dataplane (switchdev).
 */
class QosMgr : public Orch
{
public:
    QosMgr(DBConnector *cfgDb, DBConnector *stateDb,
           const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    /* STATE_DB tables for object status */
    Table m_stateDscpToTcTable;
    Table m_stateDot1pToTcTable;
    Table m_stateTcToQueueTable;
    Table m_stateTcToDscpTable;
    Table m_stateSchedulerTable;
    Table m_stateWredTable;
    Table m_stateQueueTable;
    Table m_statePortQosMapTable;

    /* In-memory map definitions: name -> {field -> value} */
    std::map<std::string, std::map<std::string, std::string>> m_dscpToTcMap;
    std::map<std::string, std::map<std::string, std::string>> m_dot1pToTcMap;
    std::map<std::string, std::map<std::string, std::string>> m_tcToQueueMap;
    std::map<std::string, std::map<std::string, std::string>> m_tcToDscpMap;
    std::map<std::string, std::map<std::string, std::string>> m_schedulerMap;
    std::map<std::string, std::map<std::string, std::string>> m_wredMap;

    /* In-memory bindings */
    std::map<std::string, std::map<std::string, std::string>> m_portQosMap; // port -> {field -> map name}
    std::map<std::string, std::map<std::string, std::string>> m_queueMap;   // "port|queue" -> {scheduler/wred}

    void doTask(Consumer &consumer);

    /* Per-table handlers (map definitions) */
    void doDscpToTcTask(Consumer &consumer);
    void doDot1pToTcTask(Consumer &consumer);
    void doTcToQueueTask(Consumer &consumer);
    void doTcToDscpTask(Consumer &consumer);
    void doSchedulerTask(Consumer &consumer);
    void doWredTask(Consumer &consumer);

    /* Per-table handlers (bindings) */
    void doPortQosMapTask(Consumer &consumer);
    void doQueueTask(Consumer &consumer);

    /* Generic map-definition helpers */
    bool validateNumMap(const std::vector<KeyOpFieldsValuesTuple> &fields,
                        uint32_t keyMax, uint32_t valMax, std::string &reason);
    void writeMapStatus(Table &stateTable, const std::string &name,
                        const std::string &status);

    /* Reference resolution */
    bool resolvePortQosField(const std::string &field, const std::string &mapName,
                             std::string &reason);

    /* Kernel programming */
    bool interfaceExists(const std::string &iface);
    bool applyMapsToPort(const std::string &iface,
                         const std::map<std::string, std::string> &maps,
                         std::string &reason);
    bool applyQueueToPort(const std::string &iface, const std::string &queue,
                          const std::string &scheduler, const std::string &wred,
                          std::string &reason);
};

} // namespace swss

#endif /* __QOSMGR__ */
