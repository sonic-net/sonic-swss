#include "common/redisreply.h"
#include "common/producertable.h"
#include "common/json.h"
#include <stdlib.h>
#include <tuple>

using namespace std;

namespace swss {

ProducerTable::ProducerTable(DBConnector *db, string tableName) :
    Table(db, tableName)
{
}

void ProducerTable::enqueueDbChange(string key, string value, string op)
{
    string lpush_value;
    string lpush_key("LPUSH ");
    string lpush_op = lpush_key;
    char *temp;
    int len;

    lpush_key += getKeyQueueTableName();
    lpush_key += " ";
    lpush_key += key;
    enqueue(lpush_key, REDIS_REPLY_INTEGER);

    len = redisFormatCommand(&temp, "LPUSH %s %s", getValueQueueTableName().c_str(), value.c_str());
    lpush_value = string(temp, len);
    free(temp);
    enqueue(lpush_value, REDIS_REPLY_INTEGER, true);

    lpush_op += getOpQueueTableName();
    lpush_op += " ";
    lpush_op += op;
    enqueue(lpush_op, REDIS_REPLY_INTEGER);

    string publish("PUBLISH ");
    publish += getChannelTableName();
    publish += " G";
    enqueue(publish, REDIS_REPLY_INTEGER);
}

void ProducerTable::set(string key, vector<FieldValueTuple> &values, string op)
{
    multi();
    for (FieldValueTuple &i : values)
        enqueue(formatHSET(getKeyName(key), fvField(i), fvValue(i)),
                REDIS_REPLY_INTEGER, true);

    enqueueDbChange(key, JSon::buildJson(values), op);
    exec();
}

void ProducerTable::del(string key, string op)
{
    string del("DEL ");
    del += getKeyName(key);

    multi();

    enqueueDbChange(key, "{}", op);
    enqueue(del, REDIS_REPLY_INTEGER);

    exec();
}

}
