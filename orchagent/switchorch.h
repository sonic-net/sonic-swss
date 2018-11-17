#pragma once

#include "orch.h"
#include "producertable.h"

#define SWITCH_SENSORS_FLEX_GROUP "SWITCH_SENSORS"

struct WarmRestartCheck
{
    bool    checkRestartReadyState;
    bool    noFreeze;
    bool    skipPendingTaskCheck;
};

class SwitchOrch : public Orch
{
public:
    SwitchOrch(DBConnector *db, string tableName);

    bool checkRestartReady() { return m_warmRestartCheck.checkRestartReadyState; }
    bool checkRestartNoFreeze() { return m_warmRestartCheck.noFreeze; }
    bool skipPendingTaskCheck() { return m_warmRestartCheck.skipPendingTaskCheck; }
    void checkRestartReadyDone() { m_warmRestartCheck.checkRestartReadyState = false; }
    void restartCheckReply(const string &op, const string &data, std::vector<FieldValueTuple> &values);
    bool setAgingFDB(uint32_t sec);
private:
    void doTask(Consumer &consumer);
    std::string  getSwitchSensorsFlexCounterTableKey(std::string);
    void addSwitchSensorsToFlexCounters();

    NotificationConsumer* m_restartCheckNotificationConsumer;
    void doTask(NotificationConsumer& consumer);
    DBConnector *m_db;

    shared_ptr<DBConnector> m_flex_db = nullptr;
    shared_ptr<ProducerTable> m_flexCounterTable = nullptr;
    shared_ptr<ProducerTable> m_flexCounterGroupTable = nullptr;

    // Information contained in the request from
    // external program for orchagent pre-shutdown state check
    WarmRestartCheck m_warmRestartCheck = {false, false, false};
};
