#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(NotificationConsumer *select, Orch *orch, const std::string& notifier_channel)
        : Executor(select, orch), m_channel(notifier_channel)
    {
    }

    NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<NotificationConsumer *>(getSelectable());
    }

    void execute()
    {
        m_orch->doTask(*getNotificationConsumer(), m_channel);
    }

private:
    std::string m_channel;
};
