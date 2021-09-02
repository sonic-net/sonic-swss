#pragma once

#include "orch.h"
#include "timer.h"

#define DEFAULT_ASIC_SENSORS_POLLER_INTERVAL 60
#define ASIC_SENSORS_POLLER_STATUS "ASIC_SENSORS_POLLER_STATUS"
#define ASIC_SENSORS_POLLER_INTERVAL "ASIC_SENSORS_POLLER_INTERVAL"

#define SWITCH_CAPABILITY_TABLE_PORT_TPID_CAPABLE                      "PORT_TPID_CAPABLE"
#define SWITCH_CAPABILITY_TABLE_LAG_TPID_CAPABLE                       "LAG_TPID_CAPABLE"

struct WarmRestartCheck
{
    bool    checkRestartReadyState;
    bool    noFreeze;
    bool    skipPendingTaskCheck;
};

class SwitchOrch : public Orch
{
public:
    SwitchOrch(swss::DBConnector *db, std::vector<TableConnector>& connectors, TableConnector switchTable);
    bool checkRestartReady() { return m_warmRestartCheck.checkRestartReadyState; }
    bool checkRestartNoFreeze() { return m_warmRestartCheck.noFreeze; }
    bool skipPendingTaskCheck() { return m_warmRestartCheck.skipPendingTaskCheck; }
    void checkRestartReadyDone() { m_warmRestartCheck.checkRestartReadyState = false; }
    void restartCheckReply(const std::string &op, const std::string &data, std::vector<swss::FieldValueTuple> &values);
    bool setAgingFDB(uint32_t sec);
    void set_switch_capability(const std::vector<swss::FieldValueTuple>& values);
private:
    void doTask(Consumer &consumer);
    void doTask(swss::SelectableTimer &timer);
    void doCfgSensorsTableTask(Consumer &consumer);
    void doAppSwitchTableTask(Consumer &consumer);
    void initSensorsTable();
    void querySwitchTpidCapability();
    sai_status_t setSwitchTunnelVxlanParams(swss::FieldValueTuple &val);

    swss::NotificationConsumer* m_restartCheckNotificationConsumer;
    void doTask(swss::NotificationConsumer& consumer);
    swss::DBConnector *m_db;
    swss::Table m_switchTable;
    sai_object_id_t switch_tunnel_id;

    // ASIC temperature sensors
    std::shared_ptr<swss::DBConnector> m_stateDb = nullptr;
    std::shared_ptr<swss::Table> m_asicSensorsTable= nullptr;
    swss::SelectableTimer* m_sensorsPollerTimer = nullptr;
    bool m_sensorsPollerEnabled = false;
    time_t m_sensorsPollerInterval = DEFAULT_ASIC_SENSORS_POLLER_INTERVAL;
    bool m_sensorsPollerIntervalChanged = false;
    uint8_t m_numTempSensors = 0;
    bool m_numTempSensorsInitialized = false;
    bool m_sensorsMaxTempSupported = true;
    bool m_sensorsAvgTempSupported = true;
    bool m_vxlanSportUserModeEnabled = false;

    // Information contained in the request from
    // external program for orchagent pre-shutdown state check
    WarmRestartCheck m_warmRestartCheck = {false, false, false};
};
