#pragma once

#include <map>
#include <memory>
#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"

using namespace std;
using namespace swss;

#define DEFAULT_KEY_SEPARATOR           ":"
#define CONFIGDB_KEY_SEPARATOR          "|"

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(TableConsumable* consumer) : m_consumer(consumer)  { }
    TableConsumable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

class CfgOrch
{
public:
    CfgOrch(DBConnector *db, string tableName);
    CfgOrch(DBConnector *db, vector<string> &tableNames);
    virtual ~CfgOrch();

    vector<Selectable*> getSelectables();
    bool hasSelectable(TableConsumable* s) const;

    bool execute(string tableName);
    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();

protected:
    DBConnector *m_db;
    ConsumerMap m_consumerMap;

    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    string dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    bool syncCfgDB(string tableName, Table &tableConsumer);
};

