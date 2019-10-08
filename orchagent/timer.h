#pragma once

#include "selectable.h"
#include "orch.h"

namespace swss {

class ExecutableTimer : public Executor
{
public:
    ExecutableTimer(swss::SelectableTimer *timer, Orch *orch, const std::string &name)
        : Executor(timer, orch, name)
    {
    }

    swss::SelectableTimer *getSelectableTimer()
    {
        return static_cast<swss::SelectableTimer *>(getSelectable());
    }

    void execute()
    {
        m_orch->doTask(*getSelectableTimer());
    }
};

}

