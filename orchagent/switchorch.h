#pragma once

#include "orch.h"

class SwitchOrch : public Orch
{
public:
    SwitchOrch(DBConnector *db, string tableName);

private:
    void doTask(Consumer &consumer);
};
