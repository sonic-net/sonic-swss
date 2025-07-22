#pragma once

#include "orch.h"

class ObjectManagerInterface
{
  public:
    virtual ~ObjectManagerInterface() = default;

    // Enqueues an entry into the manager
    virtual void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) = 0;

    // Processes all entries in the queue
    // Stops on first failure, returns first error status.
    virtual ReturnCode drain() = 0;
 
    // Drains all entries in the queue without execution.
    virtual void drainWithNotExecuted() = 0;

    // StateVerification helper function for the manager
    virtual std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) = 0;

    // For sai extension objects depending on a sai object
    // return sai object id for a given table with a given key
    virtual ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                    std::string &object_key) = 0;
};
