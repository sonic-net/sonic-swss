#ifndef SWSS_ORCH_H
#define SWSS_ORCH_H

#include <map>
#include <memory>

extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"
#include "consumerstatetable.h"

using namespace std;
using namespace swss;

const char delimiter           = ':';
const char list_item_delimiter = ',';
const char ref_start           = '[';
const char ref_end             = ']';
const char comma               = ',';
const char range_specifier     = '-';

#define MLNX_PLATFORM_SUBSTRING "mellanox"
#define BRCM_PLATFORM_SUBSTRING "broadcom"

typedef enum
{
    task_success,
    task_invalid_entry,
    task_failed,
    task_need_retry,
    task_ignore
} task_process_status;

typedef map<string, sai_object_id_t> object_map;
typedef pair<string, sai_object_id_t> object_map_pair;

typedef map<string, object_map*> type_map;
typedef pair<string, object_map*> type_map_pair;

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(TableConsumable* consumer) : m_consumer(consumer)  { }
    TableConsumable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

typedef enum
{
    success,
    field_not_found,
    multiple_instances,
    not_resolved,
    failure
} ref_resolve_status;

typedef pair<DBConnector *, string> TableConnector;
typedef pair<DBConnector *, vector<string>> TablesConnector;

class Orch
{
public:
    Orch(DBConnector *db, string tableName);
    Orch(DBConnector *db, vector<string> &tableNames);
    Orch(const vector<TableConnector>& tables);
    virtual ~Orch();

    vector<Selectable*> getSelectables();
    bool hasSelectable(TableConsumable* s) const;

    bool execute(string tableName);
    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();

protected:
    ConsumerMap m_consumerMap;

    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    void logfileReopen();
    void recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    ref_resolve_status resolveFieldRefValue(type_map&, const string&, KeyOpFieldsValuesTuple&, sai_object_id_t&);
    bool parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high);
    bool parseReference(type_map &type_maps, string &ref, string &table_name, string &object_name);
    ref_resolve_status resolveFieldRefArray(type_map&, const string&, KeyOpFieldsValuesTuple&, vector<sai_object_id_t>&);
    void addConsumer(DBConnector *db, string tableName);
};

#endif /* SWSS_ORCH_H */
