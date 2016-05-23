#ifndef SWSS_ORCH_H
#define SWSS_ORCH_H

extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include "dbconnector.h"
#include "consumertable.h"
#include "producertable.h"

#include <map>

using namespace std;
using namespace swss;

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(ConsumerTable* consumer) :m_consumer(consumer)  { }
    ConsumerTable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef std::pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

class Orch
{
public:
    Orch(DBConnector *db, string tableName);
    Orch(DBConnector *db, vector<string> &tableNames);
    ~Orch();

    std::vector<Selectable*> getConsumers();
    bool hasConsumer(ConsumerTable* s)const;

    bool reinit();
    bool execute(string tableName);

protected:
    DBConnector *m_db;
    ConsumerMap m_consumerMap;
    virtual void doTask(Consumer &consumer) = 0;
};

#endif /* SWSS_ORCH_H */
