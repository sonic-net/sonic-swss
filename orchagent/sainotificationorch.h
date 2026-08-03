#pragma once

#include "orch.h"
#include "notifications.h"

#include <functional>
#include <string>

/*
 * SaiNotificationOrch
 *
 * Drains the in-process SaiNotificationQueue on the orchagent main loop and
 * dispatches entries to handlers registered by feature orchs.
 *
 * Lifecycle:
 *
 *   - OrchDaemon constructs this orch once in ZMQ mode, before any orch that
 *     registers an in-process SAI notification handler.
 *   - Each <X>Orch that handles a queued notification calls
 *         gSaiNotificationOrch->registerHandler(op, handler, readiness)
 *     from its constructor.  Registration is a no-op when the global pointer
 *     is null (non-ZMQ mode).
 */
class SaiNotificationOrch : public Orch
{
public:
    SaiNotificationOrch();

    void registerHandler(const std::string &op,
                         SaiNotificationDispatcher::Handler handler,
                         SaiNotificationDispatcher::ReadinessPredicate ready = nullptr);

private:
    SaiNotificationQueue *m_queue;
    SaiNotificationDispatcher *m_dispatcher;
};

extern SaiNotificationOrch *gSaiNotificationOrch;
