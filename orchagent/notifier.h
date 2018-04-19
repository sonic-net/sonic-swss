#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(NotificationConsumer *select, Orch *orch)
        : Executor(select, orch)
    {
    }

    NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<NotificationConsumer *>(getSelectable());
    }

    void execute()
    {
        m_orch->doTask(*getNotificationConsumer());
    }
};
