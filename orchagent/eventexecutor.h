#pragma once

#include <sstream>

#include "selectable.h"
#include "orch.h"
#include "return_code.h"

namespace swss {

class EventExecutor : public Executor
{
public:
    EventExecutor(swss::SelectableEvent *event, Orch *orch, const std::string &name)
        : Executor(event, orch, name)
    {
    }

    swss::SelectableEvent *getSelectableEvent()
    {
        return static_cast<swss::SelectableEvent *>(getSelectable());
    }

    void execute()
    {
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
};

}
