#ifndef SWSS_ORCH_BASE_H
#define SWSS_ORCH_BASE_H

#include <map>
#include <memory>

#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"
#include "consumerstatetable.h"

using namespace std;
using namespace swss;

#define DEFAULT_BATCH_SIZE        128
#define DEFAULT_KEY_SEPARATOR     ":"
#define CONFIGDB_KEY_SEPARATOR    "|"

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(TableConsumable* consumer) : m_consumer(consumer)  { }
    TableConsumable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

typedef pair<DBConnector *, string> TableConnector;
typedef pair<DBConnector *, vector<string>> TablesConnector;

class OrchBase
{
public:
    OrchBase(DBConnector *db, string tableName);
    OrchBase(DBConnector *db, const vector<string> &tableNames);
    OrchBase(const vector<TableConnector>& tables);
    virtual ~OrchBase();

    vector<Selectable*> getSelectables();
    bool hasSelectable(TableConsumable* s) const;

    virtual bool execute(string tableName);
    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    virtual void doTask();

protected:
    ConsumerMap m_consumerMap;

    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    virtual void recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    string dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    void addConsumer(DBConnector *db, string tableName, int batchSize = DEFAULT_BATCH_SIZE);
    bool syncDB(string tableName, Table &tableConsumer);
};

#endif /* SWSS_ORCH_BASE_H */
