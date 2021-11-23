#pragma once

#include <string>
#include <unordered_set>

extern "C" {
#include "sai.h"
}

class FlowCounterHandler
{
public:
    static bool createGenericCounter(sai_object_id_t &counter_id);
    static bool removeGenericCounter(sai_object_id_t counter_id);
    static void getGenericCounterStatIdList(std::unordered_set<std::string>& counter_stats);
    static bool queryRouteFlowCounterCapability();
};
