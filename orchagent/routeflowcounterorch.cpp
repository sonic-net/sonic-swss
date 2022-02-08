#include "dbconnector.h"
#include "directory.h"
#include "flow_counter_handler.h"
#include "routeorch.h"
#include "routeflowcounterorch.h"
#include "schema.h"
#include "table.h"

#include <string>

extern Directory<Orch*> gDirectory;
extern RouteOrch *gRouteOrch;
#define ROUTE_PATTERN_MAX_MATCH_COUNT_FIELD "max_match_count"
#define ROUTE_PATTERN_DEFAULT_MAX_MATCH_COUNT 30

RouteFlowCounterOrch::RouteFlowCounterOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames):
Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
}

RouteFlowCounterOrch::~RouteFlowCounterOrch()
{
    SWSS_LOG_ENTER();
}

void RouteFlowCounterOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    if (!gRouteOrch || !gRouteOrch->getRouteFlowCounterSupported())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        const auto &key =  kfvKey(t);
        const auto &op = kfvOp(t);
        const auto &data = kfvFieldsValues(t);
        if (op == SET_COMMAND)
        {
            size_t maxMatchCount = ROUTE_PATTERN_DEFAULT_MAX_MATCH_COUNT;
            for (auto valuePair : data)
            {
                const auto &field = fvField(valuePair);
                const auto &value = fvValue(valuePair);
                if (field == ROUTE_PATTERN_MAX_MATCH_COUNT_FIELD)
                {
                    maxMatchCount = (size_t)std::stoul(value);
                    if (maxMatchCount == 0)
                    {
                        SWSS_LOG_WARN("Max match count for route pattern cannot be 0, set it to default value 30");
                        maxMatchCount = ROUTE_PATTERN_DEFAULT_MAX_MATCH_COUNT;
                    }
                }
            }

            gRouteOrch->addRoutePattern(key, maxMatchCount);
        }
        else if (op == DEL_COMMAND)
        {
            gRouteOrch->removeRoutePattern(key);
        }
        consumer.m_toSync.erase(it++);
    }
}
