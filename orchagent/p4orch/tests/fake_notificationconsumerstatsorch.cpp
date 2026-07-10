// Minimal stub for p4orch unit tests.  The real
// NotificationConsumerStatsOrch lives in
// orchagent/notificationconsumerstatsorch.cpp and is constructed by
// OrchDaemon::init().  p4orch tests don't construct an OrchDaemon,
// so we provide the global pointer as null -- the
// `if (gNotifConsumerStatsOrch)` guard in P4Orch's constructor will
// then skip registration without crashing.
//
// The linker still needs definitions of any method P4Orch::ctor
// references symbolically.  P4Orch.cpp guards every call site under
// the null check so registerConsumer is never actually invoked, but
// the symbol must resolve.  Provide no-op definitions here.

#include "../../notificationconsumerstatsorch.h"

NotificationConsumerStatsOrch *gNotifConsumerStatsOrch = nullptr;

NotificationConsumerStatsOrch::NotificationConsumerStatsOrch()
    : Orch()
{
}

void NotificationConsumerStatsOrch::registerConsumer(const std::string &,
                                                     swss::NotificationConsumer *)
{
}

void NotificationConsumerStatsOrch::doTask(swss::SelectableTimer &)
{
}
