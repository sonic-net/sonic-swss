#pragma once

#include "selectable.h"
#include "orch.h"

namespace swss {

class ExecutableTimer : public Executor
{
public:
    ExecutableTimer(SelectableTimer *timer, Orch *orch, const string &name)
        : Executor(timer, orch, name)
    {
    }

    SelectableTimer *getSelectableTimer()
    {
        return static_cast<SelectableTimer *>(getSelectable());
    }

    void execute()
    {
        m_orch->doTask(*getSelectableTimer());
    }
};

}

