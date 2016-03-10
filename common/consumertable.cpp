#include "common/redisreply.h"
#include "common/consumertable.h"
#include "common/json.h"
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <system_error>

/* The database is already alive and kicking, no need for more than a second */
#define SUBSCRIBE_TIMEOUT (1000)

using namespace std;

namespace swss {

ConsumerTable::ConsumerTable(DBConnector *db, string tableName) :
    Table(db, tableName),
    m_subscribe(NULL),
    m_queueLength(0)
{
    bool again = true;

    while (again)
    {
        try
        {
            RedisReply watch(m_db, string("WATCH ") + getKeyQueueTableName(), REDIS_REPLY_STATUS);
            watch.checkStatusOK();
            multi();
            enqueue(string("LLEN ") + getKeyQueueTableName(), REDIS_REPLY_INTEGER);
            subsribe();
            enqueue(string("LLEN ") + getKeyQueueTableName(), REDIS_REPLY_INTEGER);
            exec();
            again = false;
        }
        catch (...)
        {
            delete m_subscribe;
        }
    }

    m_queueLength = queueResultsFront()->integer;
    /* No need for that since we have WATCH gurantee on the transaction */
}

ConsumerTable::~ConsumerTable()
{
    delete m_subscribe;
}

static string pop_front(queue<RedisReply *> &q)
{
    string ret(q.front()->getContext()->str);
    delete q.front();
    q.pop();
    return ret;
}

void ConsumerTable::pop(KeyOpFieldsValuesTuple &kco)
{
    string rpop_key("RPOP ");
    string rpop_value = rpop_key;
    string rpop_op = rpop_key;

    multi();

    rpop_key += getKeyQueueTableName();
    enqueue(rpop_key, REDIS_REPLY_STRING);

    rpop_op += getOpQueueTableName();
    enqueue(rpop_op, REDIS_REPLY_STRING);

    rpop_value += getValueQueueTableName();
    enqueue(rpop_value, REDIS_REPLY_STRING);

    exec();

    vector<FieldValueTuple> fieldsValues;
    string key = pop_front(m_results);
    string op = pop_front(m_results);
    JSon::readJson(pop_front(m_results), fieldsValues);

    kco = std::make_tuple(key, op, fieldsValues);
}

void ConsumerTable::addFd(fd_set *fd)
{
    FD_SET(m_subscribe->getContext()->fd, fd);
}

int ConsumerTable::readCache()
{
    redisReply *reply = NULL;

    /* Read the messages in queue before subsribe command execute */
    if (m_queueLength) {
        m_queueLength--;
        return ConsumerTable::DATA;
    }

    if (redisGetReplyFromReader(m_subscribe->getContext(),
                                (void**)&reply) != REDIS_OK)
    {
        return Selectable::ERROR;
    } else if (reply != NULL)
    {
        freeReplyObject(reply);
        return Selectable::DATA;
    }

    return Selectable::NODATA;
}

void ConsumerTable::readMe()
{
    redisReply *reply = NULL;

    if (redisGetReply(m_subscribe->getContext(), (void**)&reply) != REDIS_OK)
        throw "Unable to read redis reply";

    freeReplyObject(reply);
}

bool ConsumerTable::isMe(fd_set *fd)
{
    return FD_ISSET(m_subscribe->getContext()->fd, fd);
}

void ConsumerTable::subsribe()
{
    /* Create new new context to DB */
    if (m_db->getContext()->connection_type == REDIS_CONN_TCP)
        m_subscribe = new DBConnector(m_db->getDB(),
                                      m_db->getContext()->tcp.host,
                                      m_db->getContext()->tcp.port,
                                      SUBSCRIBE_TIMEOUT);
    else
        m_subscribe = new DBConnector(m_db->getDB(),
                                      m_db->getContext()->unix_sock.path,
                                      SUBSCRIBE_TIMEOUT);
    /* Send SUBSCRIBE #channel command */
    string s("SUBSCRIBE ");
    s+= getChannelTableName();
    RedisReply r(m_subscribe, s, REDIS_REPLY_ARRAY);
}

}
