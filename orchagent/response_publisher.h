#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dbconnector.h"
#include "notificationproducer.h"
#include "recorder.h"
#include "response_publisher_interface.h"
#include "table.h"

// This class performs two tasks when publish is called:
// 1. Sends a notification into the redis channel.
// 2. Writes the operation into the DB.
class ResponsePublisher : public ResponsePublisherInterface
{
  public:
    explicit ResponsePublisher(bool buffered = false);

    virtual ~ResponsePublisher() = default;

    // Intent attributes are the attributes sent in the notification into the
    // redis channel.
    // State attributes are the list of attributes that need to be written in
    // the DB namespace. These might be different from intent attributes. For
    // example:
    // 1) If only a subset of the intent attributes were successfully applied, the
    //    state attributes shall be different from intent attributes.
    // 2) If additional state changes occur due to the intent attributes, more
    //    attributes need to be added in the state DB namespace.
    // 3) Invalid attributes are excluded from the state attributes.
    // State attributes will be written into the DB even if the status code
    // consists of an error.
    void publish(const std::string &table, const std::string &key,
                 const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                 const std::vector<swss::FieldValueTuple> &state_attrs, bool replace = false) override;

    void publish(const std::string &table, const std::string &key,
                 const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                 bool replace = false) override;

    void writeToDB(const std::string &table, const std::string &key, const std::vector<swss::FieldValueTuple> &values,
                   const std::string &op, bool replace = false) override;

    /**
     * @brief Flush pending responses
     */
    void flush();

    /**
     * @brief Set buffering mode
     *
     * @param buffered Flag whether responses are buffered
     */
    void setBuffered(bool buffered);

  private:
    std::unique_ptr<swss::DBConnector> m_db;
    std::unique_ptr<swss::RedisPipeline> m_pipe;

    bool m_buffered{false};
};
