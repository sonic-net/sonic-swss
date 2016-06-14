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

const char delimiter           = ':';
const char list_item_delimiter = ',';

typedef enum
{
    task_success,
    task_invalid_entry,
    task_failed,
    task_need_retry
} task_process_status;

typedef std::map<string, sai_object_id_t> object_map;
typedef std::pair<string, sai_object_id_t> object_map_pair;
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
    virtual ~Orch();

    std::vector<Selectable*> getSelectables();
    bool hasSelectable(ConsumerTable* s) const;

    bool execute(string tableName);
    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();
protected:
    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    void dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
private:
    DBConnector *m_db;

protected:
    ConsumerMap m_consumerMap;

};

#endif /* SWSS_ORCH_H */
