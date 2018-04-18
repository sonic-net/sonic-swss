#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(NotificationConsumer *select, Orch *orch, const std::string& notifier_name)
        : Executor(select, orch), m_name(notifier_name)
    {
    }

    NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<NotificationConsumer *>(getSelectable());
    }

    void execute()
    {
        m_orch->doTask(*getNotificationConsumer(), m_name);
    }

private:
    std::string m_name;
};
