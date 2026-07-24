#pragma once

#include <sstream>

#include "selectable.h"
#include "selectableevent.h"
#include "orch.h"
#include "logger.h"

namespace swss {

class EventExecutor : public Executor
{
public:
    EventExecutor(swss::SelectableEvent *event, Orch *orch, const std::string &name)
        : Executor(event, orch, name)
    {
        assert(event != nullptr);
    }

    swss::SelectableEvent *getSelectableEvent()
    {
        return static_cast<swss::SelectableEvent *>(getSelectable());
    }

    void execute() override
    {
        /*
         * Note: Unlike ExecutableTimer or Notifier, this executor explicitly catches
         * exceptions. This is intentional for critical events (such as watchport)
         * where doTask() may throw std::runtime_error to signal unrecoverable state
         * inconsistencies. Catching them here allows the agent to notify the system
         * via SWSS_RAISE_CRITICAL_STATE, enabling a managed recovery (e.g., warm
         * reboot) rather than an unmanaged process crash.
         */
        try
        {
            m_orch->doTask(*getSelectableEvent());
        }
        catch (const std::runtime_error &e)
        {
            std::stringstream msg;
            msg << "Received runtime_error exception: " << e.what();
            SWSS_RAISE_CRITICAL_STATE(msg.str());
        }
    }

    void drain() override
    {
        execute();
    }
};

}
