#pragma once

#include <string>

#include "return_code.h"
#include "table.h"

class ResponsePublisherInterface
{
  public:
    virtual ~ResponsePublisherInterface() = default;

    // Publishes the response status.
    // If intent attributes are empty, it is a delete operation.
    // What "publish" needs to do is completely up to implementation.
    // This API does not include redis DB namespace. So if implementation chooses
    // to write to a redis DB, it will need to use a fixed namespace.
    // The replace flag indicates the state attributes will replace the old ones.
    virtual void publish(const std::string &table, const std::string &key,
                         const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                         const std::vector<swss::FieldValueTuple> &state_attrs, bool replace = false) = 0;

    // Publishes response status. If response status is OK then also writes the
    // intent attributes into the DB.
    // The replace flag indicates a replace operation.
    virtual void publish(const std::string &table, const std::string &key,
                         const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                         bool replace = false) = 0;

    // Write to DB only. This API does not send notification.
    // The replace flag indicates the new attributes will replace the old ones.
    virtual void writeToDB(const std::string &table, const std::string &key,
                           const std::vector<swss::FieldValueTuple> &values, const std::string &op,
                           bool replace = false) = 0;
};
