#include <hiredis/hiredis.h>
#include <system_error>

#include "common/table.h"
#include "common/logger.h"
#include "common/redisreply.h"

using namespace std;

namespace swss {

Table::Table(DBConnector *db, string tableName) :
    m_db(db),
    m_tableName(tableName)
{
}

string Table::getKeyName(string key)
{
    return m_tableName + ':' + key;
}

string Table::getKeyQueueTableName()
{
    return m_tableName + "_KEY_QUEUE";
}

string Table::getValueQueueTableName()
{
    return m_tableName + "_VALUE_QUEUE";
}

string Table::getOpQueueTableName()
{
    return m_tableName + "_OP_QUEUE";
}

string Table::getChannelTableName()
{
    return m_tableName + "_CHANNEL";
}

bool Table::get(std::string key, vector<FieldValueTuple> &values)
{
    string hgetall_key("HGETALL ");
    hgetall_key += getKeyName(key);

    RedisReply r(m_db, hgetall_key, REDIS_REPLY_ARRAY);
    redisReply *reply = r.getContext();
    values.clear();

    if (!reply->elements)
        return false;

    if (reply->elements & 1)
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect netlink socket");

    for (unsigned int i = 0; i < reply->elements; i += 2)
        values.push_back(make_tuple(reply->element[i]->str,
                                    reply->element[i + 1]->str));

    return true;
}

void Table::set(std::string key, std::vector<FieldValueTuple> &values,
                std::string /*op*/)
{
    /* We are doing transaction for AON (All or nothing) */
    multi();
    for (FieldValueTuple &i : values)
        enqueue(formatHSET(getKeyName(key), fvField(i), fvValue(i)),
                REDIS_REPLY_INTEGER, true);

    exec();
}

void Table::del(std::string key, std::string /* op */)
{
    RedisReply r(m_db, string("DEL ") + getKeyName(key), REDIS_REPLY_INTEGER);
    if (r.getContext()->type != REDIS_REPLY_INTEGER)
        throw system_error(make_error_code(errc::io_error),
                           "DEL operation failed");
}

void Table::multi()
{
    while (!m_expectedResults.empty())
        m_expectedResults.pop();
    RedisReply r(m_db, "MULTI", REDIS_REPLY_STATUS);
    r.checkStatusOK();
}

redisReply *Table::queueResultsFront()
{
    return m_results.front()->getContext();
}

void Table::queueResultsPop()
{
    delete m_results.front();
    m_results.pop();
}

void Table::exec()
{
    redisReply *reply = (redisReply *)redisCommand(m_db->getContext(), "EXEC");
    unsigned int size = reply->elements;

    try
    {
        if (reply->type != REDIS_REPLY_ARRAY)
            throw system_error(make_error_code(errc::io_error),
                               "Error in transaction");

        if (size != m_expectedResults.size())
            throw system_error(make_error_code(errc::io_error),
                               "Got to different nuber of answers!");

        while (!m_results.empty())
            queueResultsPop();

        for (unsigned int i = 0; i < size; i++)
        {
            int expectedType = m_expectedResults.front();
            m_expectedResults.pop();
            if (expectedType != reply->element[i]->type)
            {
                SWSS_LOG_INFO("Except to get redis type %d got type %d\n",
                              expectedType, reply->element[i]->type);
                throw system_error(make_error_code(errc::io_error),
                                   "Got unexpected result");
            }
        }
    }
    catch (...)  {
        freeReplyObject(reply);
        throw;
    }

    for (unsigned int i = 0; i < size; i++)
        m_results.push(new RedisReply(reply->element[i]));

    /* Free only the array memory */
    free(reply->element);
    free(reply);
}

void Table::enqueue(std::string command, int exepectedResult, bool isFormatted)
{
    RedisReply r(m_db, command, REDIS_REPLY_STATUS, isFormatted);
    r.checkStatusQueued();
    m_expectedResults.push(exepectedResult);
}

string Table::formatHSET(const string& key, const string& field,
                         const string& value)
{
        char *temp;
        int len = redisFormatCommand(&temp, "HSET %s %s %s",
                                     key.c_str(),
                                     field.c_str(),
                                     value.c_str());
        string hset(temp, len);
        free(temp);
        return hset;
}

}
