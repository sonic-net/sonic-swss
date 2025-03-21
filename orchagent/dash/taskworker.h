#pragma once

#include <functional>
#include <memory>
#include <map>
#include <string>
#include <tuple>

#include <google/protobuf/message.h>

#include <swss/logger.h>
#include <swss/redisutility.h>
#include <swss/rediscommand.h>

#include "orchagent/orch.h"

class TaskWorker
{
public:
    virtual task_process_status process(
        const std::string &key,
        const std::vector<swss::FieldValueTuple> &data) = 0;
};

using TaskKey = std::tuple<const std::string, const std::string>;
using TaskFunc = std::shared_ptr<TaskWorker>;
using TaskMap = std::map<TaskKey, TaskFunc>;

#define PbIdentifier "pb"

template<typename MessageType>
bool parsePbMessage(
    const std::vector<swss::FieldValueTuple> &data,
    MessageType &msg)
{
    SWSS_LOG_ENTER();

    auto pb = swss::fvsGetValue(data, PbIdentifier);
    if (pb)
    {
        if (msg.ParseFromString(*pb))
        {
            return true;
        }
        else
        {
            SWSS_LOG_WARN("Failed to parse protobuf message from string: %s", pb->c_str());
        }
    }
    else
    {
        SWSS_LOG_WARN("Protobuf field cannot be found");
    }

    return false;
}

template<typename MessageType>
class PbWorker : public TaskWorker
{
public:
    using Task = std::function<task_process_status(const std::string &, const MessageType &)>;

    PbWorker(const Task &func) : m_func(func) {}

    virtual task_process_status process(
            const std::string &key,
            const std::vector<swss::FieldValueTuple> &data)
    {
        SWSS_LOG_ENTER();

        MessageType msg;
        if (parsePbMessage(data, msg))
        {
            return m_func(key, msg);
        }
        else
        {
            SWSS_LOG_WARN("This orch requires protobuff message at :%s", key.c_str());
        }

        return task_process_status::task_invalid_entry;
    }

    template<typename MemberFunc, typename ObjType>
    static TaskMap::value_type makeMemberTask(
        const std::string &table,
        const std::string &op,
        MemberFunc func,
        ObjType *obj)
    {
        return std::make_pair(
            std::make_tuple(table, op),
            std::make_shared<PbWorker<MessageType> >(
                std::bind(func, obj, std::placeholders::_1, std::placeholders::_2)));
    }

private:
     Task m_func;
};

class KeyOnlyWorker : public TaskWorker
{
public:
    using Task = std::function<task_process_status(const std::string &)>;

    KeyOnlyWorker(const Task &func) : m_func(func) {}

    virtual task_process_status process(
            const std::string &key,
            const std::vector<swss::FieldValueTuple> &data)
    {
        SWSS_LOG_ENTER();

        return m_func(key);
    }

    template<typename MemberFunc, typename ObjType>
    static TaskMap::value_type makeMemberTask(
        const std::string &table,
        const std::string &op,
        MemberFunc func,
        ObjType *obj)
    {
        return std::make_pair(
            std::make_tuple(table, op),
            std::make_shared<KeyOnlyWorker>(
                std::bind(func, obj, std::placeholders::_1)));
    }

private:
     Task m_func;
};
