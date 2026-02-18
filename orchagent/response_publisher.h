#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dbconnector.h"
#include "notificationproducer.h"
#include "recorder.h"
#include "response_publisher_interface.h"
#include "table.h"
#include "zmqserver.h"

// This class performs two tasks when publish is called:
// 1. Sends a notification into the redis channel.
// 2. Writes the operation into the DB.
class ResponsePublisher : public ResponsePublisherInterface
{
  public:
    explicit ResponsePublisher(const std::string& dbName, bool buffered = false,
                               bool db_write_thread = false,
                             swss::ZmqServer* zmqServer = nullptr);
    virtual ~ResponsePublisher();

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
    void flush(bool warmboot = false);

    /**
     * @brief Set buffering mode
     *
     * @param buffered Flag whether responses are buffered
     */
    void setBuffered(bool buffered);


    // When true, write attributes directly to DB without merge logic.
    // When false (default), check for existing keys and filter NULL-valued attributes.
    bool m_directDbWrite = false;

  private:
    struct entry
    {
        std::string table;
        std::string key;
        std::vector<swss::FieldValueTuple> values;
        std::string op;
        bool replace;
        bool flush;
        bool shutdown;

        entry()
        {
        }

        entry(const std::string &table, const std::string &key, const std::vector<swss::FieldValueTuple> &values,
              const std::string &op, bool replace, bool flush, bool shutdown)
            : table(table), key(key), values(values), op(op), replace(replace), flush(flush), shutdown(shutdown)
        {
        }
    };

    void dbUpdateThread();
    void writeToDBInternal(const std::string &table, const std::string &key,
                           const std::vector<swss::FieldValueTuple> &values, const std::string &op, bool replace);

    std::unique_ptr<swss::DBConnector> m_db;
    std::unique_ptr<swss::RedisPipeline> m_ntf_pipe;
    std::unique_ptr<swss::RedisPipeline> m_db_pipe;

    bool m_buffered{false};
  swss::ZmqServer* m_zmqServer;
  std::unordered_map<std::string, std::vector<swss::KeyOpFieldsValuesTuple>>
      responses;  // Cache the responses to send them together in flush(). Only
                  // used when ZMQ is enabled.
    // Thread to write to DB.
    std::unique_ptr<std::thread> m_update_thread;
    std::queue<entry> m_queue;
    mutable std::mutex m_lock;
    std::condition_variable m_signal;
};
