#pragma once

#include "orch.h"

struct WarmRestartCheck
{
    bool    checkRestartReadyState;
    bool    noFreeze;
    bool    pendingTaskCheck;
};

class SwitchOrch : public Orch
{
public:
    SwitchOrch(DBConnector *db, string tableName);

    bool checkRestartReady() { return m_warmRestartCheck.checkRestartReadyState; }
    bool checkRestartNoFreeze() { return m_warmRestartCheck.noFreeze; }
    bool skipPendingTaskCheck() { return m_warmRestartCheck.pendingTaskCheck; }
    void checkRestartReadyDone() { m_warmRestartCheck.checkRestartReadyState = false; }
    void restartCheckReply(const string &op, const string &data, std::vector<FieldValueTuple> &values);
private:
    void doTask(Consumer &consumer);

    NotificationConsumer* m_restartCheckNotificationConsumer;
    void doTask(NotificationConsumer& consumer);
    DBConnector *m_db;

    // Information contained in the request from
    // external program for orchagent pre-shutdown state check
    WarmRestartCheck m_warmRestartCheck;
};
