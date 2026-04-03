#include "response_publisher.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "timestamp.h"

namespace
{

// Returns the component string that we need to prepend for sending the error
// message.
// Returns an empty string if the status is OK.
// Returns "[SAI] " if the ReturnCode is generated from a SAI status code.
// Else, returns "[OrchAgent] ".
std::string PrependedComponent(const ReturnCode &status)
{
    constexpr char *kOrchagentComponent = "[OrchAgent] ";
    constexpr char *kSaiComponent = "[SAI] ";
    if (status.ok())
    {
        return "";
    }
    if (status.isSai())
    {
        return kSaiComponent;
    }
    return kOrchagentComponent;
}

void RecordDBWrite(const std::string &table, const std::string &key, const std::vector<swss::FieldValueTuple> &attrs,
                   const std::string &op, const std::string *record_ts = nullptr)
{
    if (!swss::Recorder::Instance().respub.isRecord())
    {
        return;
    }

    std::string s = table + ":" + key + "|" + op;
    for (const auto &attr : attrs)
    {
        s += "|" + fvField(attr) + ":" + fvValue(attr);
    }

    if (record_ts != nullptr && !record_ts->empty())
    {
        swss::Recorder::Instance().respub.record(*record_ts, s);
    }
    else
    {
        swss::Recorder::Instance().respub.record(s);
    }
}

void RecordResponse(const std::string &response_channel, const std::string &key,
                    const std::vector<swss::FieldValueTuple> &attrs, const std::string &status,
                    const std::string *record_ts = nullptr)
{
    if (!swss::Recorder::Instance().respub.isRecord())
    {
        return;
    }

    std::string s = response_channel + ":" + key + "|" + status;
    for (const auto &attr : attrs)
    {
        s += "|" + fvField(attr) + ":" + fvValue(attr);
    }

    if (record_ts != nullptr && !record_ts->empty())
    {
        swss::Recorder::Instance().respub.record(*record_ts, s);
    }
    else
    {
        swss::Recorder::Instance().respub.record(s);
    }
}

} // namespace

ResponsePublisher::ResponsePublisher(const std::string& dbName, bool buffered,
                                     bool db_write_thread,
                                     swss::ZmqServer* zmqServer)
    : m_db(std::make_unique<swss::DBConnector>(dbName, 0)),
      m_ntf_pipe(std::make_unique<swss::RedisPipeline>(m_db.get())),
      m_db_pipe(std::make_unique<swss::RedisPipeline>(m_db.get())),
      m_buffered(buffered),
      m_zmqServer(zmqServer)
{
    if (db_write_thread)
    {
        m_update_thread = std::unique_ptr<std::thread>(new std::thread(&ResponsePublisher::dbUpdateThread, this));
    }
}

ResponsePublisher::~ResponsePublisher()
{
    if (m_update_thread != nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_queue.emplace(/*table=*/"", /*key=*/"", /*values =*/std::vector<swss::FieldValueTuple>{}, /*op=*/"",
                            /*replace=*/false, /*flush=*/false, /*shutdown=*/true);
        }
        m_signal.notify_one();
        m_update_thread->join();
    }
}

void ResponsePublisher::publish(const std::string &table, const std::string &key,
                                const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                                const std::vector<swss::FieldValueTuple> &state_attrs, bool replace)
{
    auto intent_attrs_copy = intent_attrs;
    // Add error message as the first field-value-pair.
    swss::FieldValueTuple err_str("err_str", PrependedComponent(status) + status.message());
    intent_attrs_copy.insert(intent_attrs_copy.begin(), err_str);
    std::string response_channel = "APPL_DB_" + table + "_RESPONSE_CHANNEL";

    if (m_enable_db_write_and_notify) {
      if (m_zmqServer != nullptr) {
        auto intent_attrs_zmq_copy = intent_attrs;
        // Add status code and error message as the first field-value-pair.
        swss::FieldValueTuple fvs(status.codeStr(),
                                  PrependedComponent(status) + status.message());
        intent_attrs_zmq_copy.insert(intent_attrs_zmq_copy.begin(), fvs);
        // Queue the response.
        responses[table].push_back(
            swss::KeyOpFieldsValuesTuple{key, SET_COMMAND, intent_attrs_zmq_copy});
      } else {
        // Sends the response to the notification channel.
        swss::NotificationProducer notificationProducer{
            m_ntf_pipe.get(), response_channel, m_buffered};
        notificationProducer.send(status.codeStr(), key, intent_attrs_copy);
      }
    }

    RecordResponse(response_channel, key, intent_attrs_copy, status.codeStr());

    // Write to the DB only if: m_enable_db_write_and_notify is true and:
    // 1) A write operation is being performed and state attributes are specified.
    // 2) OR a successful delete operation.
    if (m_enable_db_write_and_notify &&
         ((intent_attrs.size() && state_attrs.size()) ||
         (status.ok() && !intent_attrs.size()))) {
            writeToDB(table, key, state_attrs, intent_attrs.size() ? SET_COMMAND : DEL_COMMAND, replace);
    }
}

void ResponsePublisher::publish(const std::string &table, const std::string &key,
                                const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                                bool replace)
{
    // If status is OK then intent attributes need to be written in
    // APPL_STATE_DB. In this case, pass the intent attributes as state
    // attributes. In case of a failure status, nothing needs to be written in
    // APPL_STATE_DB.
    std::vector<swss::FieldValueTuple> state_attrs;
    if (status.ok())
    {
        state_attrs = intent_attrs;
    }
    publish(table, key, intent_attrs, status, state_attrs, replace);
}

void ResponsePublisher::setAsyncFullPublish(bool enable)
{
    m_async_full_publish = enable;
}

void ResponsePublisher::publishAsync(const std::string &table, const std::string &key,
                                     const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                                     bool replace)
{
    if (m_update_thread == nullptr)
    {
        publish(table, key, intent_attrs, status, replace);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_lock);
        std::string ts = swss::getTimestamp();
        entry e(table, key, std::vector<swss::FieldValueTuple>{}, "", replace, /*flush=*/false, /*shutdown=*/false);
        e.fullPublish = true;
        e.intent_attrs = intent_attrs;
        e.status = status;
        e.record_ts = std::move(ts);
        m_queue.push(std::move(e));
    }
    m_signal.notify_one();
}

void ResponsePublisher::publishFullFromThread(const std::string &table, const std::string &key,
                                              const std::vector<swss::FieldValueTuple> &intent_attrs,
                                              const ReturnCode &status, bool replace, const std::string &record_ts)
{
    std::vector<swss::FieldValueTuple> state_attrs;
    if (status.ok())
    {
        state_attrs = intent_attrs;
    }
    std::string response_channel = "APPL_DB_" + table + "_RESPONSE_CHANNEL";
    swss::NotificationProducer notificationProducer{m_ntf_pipe.get(), response_channel, m_buffered};

    auto intent_attrs_copy = intent_attrs;
    swss::FieldValueTuple err_str("err_str", PrependedComponent(status) + status.message());
    intent_attrs_copy.insert(intent_attrs_copy.begin(), err_str);
    notificationProducer.send(status.codeStr(), key, intent_attrs_copy);
    // Same enqueue-time stamp for both lines (parity with sync publish recorder timing).
    const std::string *ts_ptr = record_ts.empty() ? nullptr : &record_ts;
    RecordResponse(response_channel, key, intent_attrs_copy, status.codeStr(), ts_ptr);

    if ((intent_attrs.size() && state_attrs.size()) || (status.ok() && !intent_attrs.size()))
    {
        std::string op = intent_attrs.size() ? SET_COMMAND : DEL_COMMAND;
        writeToDBInternal(table, key, state_attrs, op, replace);
        RecordDBWrite(table, key, state_attrs, op, ts_ptr);
    }
}

void ResponsePublisher::writeToDB(const std::string &table, const std::string &key,
                                  const std::vector<swss::FieldValueTuple> &values, const std::string &op, bool replace)
{
    if (m_update_thread != nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_queue.emplace(table, key, values, op, replace, /*flush=*/false, /*shutdown=*/false);
        }
        m_signal.notify_one();
    }
    else
    {
        writeToDBInternal(table, key, values, op, replace);
    }
    RecordDBWrite(table, key, values, op);
}

void ResponsePublisher::writeToDBInternal(const std::string &table, const std::string &key,
                                          const std::vector<swss::FieldValueTuple> &values, const std::string &op,
                                          bool replace)
{
    swss::Table applStateTable{m_db_pipe.get(), table, m_buffered};

    auto attrs = values;
    if (op == SET_COMMAND)
    {
        if (m_directDbWrite)
        {
            applStateTable.set(key, attrs);
            return;
        }
        if (replace)
        {
            applStateTable.del(key);
        }
        if (!values.size())
        {
            attrs.push_back(swss::FieldValueTuple("NULL", "NULL"));
        }

        // Write to DB only if the key does not exist or non-NULL attributes are
        // being written to the entry.
        std::vector<swss::FieldValueTuple> fv;
        if (!applStateTable.get(key, fv))
        {
            applStateTable.set(key, attrs);
            return;
        }
        for (auto it = attrs.cbegin(); it != attrs.cend();)
        {
            if (it->first == "NULL")
            {
                it = attrs.erase(it);
            }
            else
            {
                it++;
            }
        }
        if (attrs.size())
        {
            applStateTable.set(key, attrs);
        }
    }
    else if (op == DEL_COMMAND)
    {
        applStateTable.del(key);
    }
}

void ResponsePublisher::flush() {
  if (m_zmqServer != nullptr) {
    for (const auto& response : responses) {
      m_zmqServer->sendMsg("APPL_DB", response.first, response.second);
    }
    responses.clear();
  } else {
    m_ntf_pipe->flush();
  }
    if (m_update_thread != nullptr)
    {
        if (!m_async_full_publish)
        {
            m_ntf_pipe->flush();
        }
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_queue.emplace(/*table=*/"", /*key=*/"", /*values =*/std::vector<swss::FieldValueTuple>{}, /*op=*/"",
                            /*replace=*/false, /*flush=*/true, /*shutdown=*/false);
        }
        m_signal.notify_one();
    }
    else
    {
        m_ntf_pipe->flush();
        m_db_pipe->flush();
    }
}

void ResponsePublisher::setBuffered(bool buffered)
{
    m_buffered = buffered;
}

// Runs on m_update_thread (response publisher db update thread).
void ResponsePublisher::dbUpdateThread()
{
    while (true)
    {
        entry e;
        {
            std::unique_lock<std::mutex> lock(m_lock);
            while (m_queue.empty())
            {
                m_signal.wait(lock);
            }

            e = m_queue.front();
            m_queue.pop();
        }
        if (e.shutdown)
        {
            break;
        }
        if (e.flush)
        {
            if (m_async_full_publish)
            {
                m_ntf_pipe->flush();
            }
            m_db_pipe->flush();
        }
        else if (e.fullPublish)
        {
            publishFullFromThread(e.table, e.key, e.intent_attrs, e.status, e.replace, e.record_ts);
        }
        else
        {
            writeToDBInternal(e.table, e.key, e.values, e.op, e.replace);
        }
    }
}

void ResponsePublisher::setEnableDbWriteAndNotify(bool enable_db_write_and_notify)
{
    m_enable_db_write_and_notify = enable_db_write_and_notify;
}

