#pragma once

#include "orch.h"

class RouteFlowCounterOrch : public Orch
{
public:
    RouteFlowCounterOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames);
    virtual ~RouteFlowCounterOrch(void);
    void doTask(Consumer &consumer) override;

private:
    void initRouteFlowCounterCapability();
};
