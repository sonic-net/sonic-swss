#include "response_publisher.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
                   const std::string &op)
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

    swss::Recorder::Instance().respub.record(s);
}

void RecordResponse(const std::string &response_channel, const std::string &key,
                    const std::vector<swss::FieldValueTuple> &attrs, const std::string &status)
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

    swss::Recorder::Instance().respub.record(s);
}

} // namespace

ResponsePublisher::ResponsePublisher(bool buffered)
    : m_db(std::make_unique<swss::DBConnector>("APPL_STATE_DB", 0)),
      m_pipe(std::make_unique<swss::RedisPipeline>(m_db.get())), m_buffered(buffered)
{
}

void ResponsePublisher::publish(const std::string &table, const std::string &key,
                                const std::vector<swss::FieldValueTuple> &intent_attrs, const ReturnCode &status,
                                const std::vector<swss::FieldValueTuple> &state_attrs, bool replace)
{
    // Write to the DB only if:
    // 1) A write operation is being performed and state attributes are specified.
    // 2) A successful delete operation.
    if ((intent_attrs.size() && state_attrs.size()) || (status.ok() && !intent_attrs.size()))
    {
        writeToDB(table, key, state_attrs, intent_attrs.size() ? SET_COMMAND : DEL_COMMAND, replace);
    }

    std::string response_channel = "APPL_DB_" + table + "_RESPONSE_CHANNEL";
    swss::NotificationProducer notificationProducer{m_pipe.get(), response_channel, m_buffered};

    auto intent_attrs_copy = intent_attrs;
    // Add error message as the first field-value-pair.
    swss::FieldValueTuple err_str("err_str", PrependedComponent(status) + status.message());
    intent_attrs_copy.insert(intent_attrs_copy.begin(), err_str);
    // Sends the response to the notification channel.
    notificationProducer.send(status.codeStr(), key, intent_attrs_copy);
    RecordResponse(response_channel, key, intent_attrs_copy, status.codeStr());
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

void ResponsePublisher::writeToDB(const std::string &table, const std::string &key,
                                  const std::vector<swss::FieldValueTuple> &values, const std::string &op, bool replace)
{
    swss::Table applStateTable{m_pipe.get(), table, m_buffered};

    auto attrs = values;
    if (op == SET_COMMAND)
    {
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
            RecordDBWrite(table, key, attrs, op);
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
            RecordDBWrite(table, key, attrs, op);
        }
    }
    else if (op == DEL_COMMAND)
    {
        applStateTable.del(key);
        RecordDBWrite(table, key, {}, op);
    }
}

void ResponsePublisher::flush()
{
    m_pipe->flush();
}

void ResponsePublisher::setBuffered(bool buffered)
{
    m_buffered = buffered;
}
