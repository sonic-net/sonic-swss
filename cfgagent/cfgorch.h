#ifndef SWSS_CFGORCH_H
#define SWSS_CFGORCH_H

#include <map>
#include <memory>

#include "dbconnector.h"
#include "consumerstatetable.h"
#include "producerstatetable.h"
#include "gettimestamp.h"

using namespace std;
using namespace swss;

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(ConsumerStateTable* consumer) :m_consumer(consumer)  { }
    ConsumerStateTable* m_consumer;
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
    bool hasSelectable(ConsumerStateTable* s) const;

    bool execute(string tableName);
    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();

protected:
    DBConnector *m_db;
    ConsumerMap m_consumerMap;

    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    void recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    string dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
};

#endif /* SWSS_CFGORCH_H */
