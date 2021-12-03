#pragma once

#include "orch.h"

class ObjectManagerInterface
{
  public:
    virtual ~ObjectManagerInterface() = default;

    // Enqueues an entry into the manager
    virtual void enqueue(const swss::KeyOpFieldsValuesTuple &entry) = 0;

    // Processes all entries in the queue
    virtual void drain() = 0;
};
