#ifndef __TABLE__
#define __TABLE__

#include <string>
#include <queue>
#include <tuple>
#include "hiredis/hiredis.h"
#include "dbconnector.h"
#include "redisreply.h"
#include "scheme.h"

namespace swss {

typedef std::tuple<std::string, std::string> FieldValueTuple;
#define fvField std::get<0>
#define fvValue std::get<1>
typedef std::tuple<std::string, std::string, std::vector<FieldValueTuple> > KeyOpFieldsValuesTuple;
#define kfvKey    std::get<0>
#define kfvOp     std::get<1>
#define kfvFieldsValues std::get<2>

class Table {
public:
    Table(DBConnector *db, std::string tableName);

    /* Read a value from the DB directly */
    /* Returns false if the key doesn't exists */
    bool get(std::string key, std::vector<FieldValueTuple> &values);

    /* Set an entry in the DB directly (op not in used) */
    virtual void set(std::string key, std::vector<FieldValueTuple> &values,
                     std::string op = "");
    /* Delete an entry in the DB directly (op not in used) */
    virtual void del(std::string key, std::string op = "");

protected:
    /* Return the actual key name as a comibation of tableName:key */
    std::string getKeyName(std::string key);

    std::string getKeyQueueTableName();
    std::string getValueQueueTableName();
    std::string getOpQueueTableName();
    std::string getChannelTableName();

    /* Start a transaction */
    void multi();
    /* Execute a transaction and get results */
    void exec();

    /* Send a command within a transaction */
    void enqueue(std::string command, int exepectedResult, bool isFormatted = false);
    redisReply* queueResultsFront();
    void queueResultsPop();

    /* Format HSET key field value command */
    static std::string formatHSET(const std::string &key,
                                  const std::string &field,
                                  const std::string &value);

    DBConnector *m_db;
    std::string m_tableName;

    /* Remember the expected results for the transaction */
    std::queue<int> m_expectedResults;
    std::queue<RedisReply * > m_results;
};

}

#endif
