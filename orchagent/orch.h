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

const char        ref_start             = '[';
const char        ref_end               = ']';
const std::string delimiter             = ":";
const std::string list_item_delimiter   = ",";
const std::string comma                 = ",";
const std::string range_specifier       = "-";

typedef std::map<string, sai_object_id_t> object_map;
typedef std::pair<string, sai_object_id_t> object_map_pair;

typedef std::map<string, object_map*> type_map;
typedef std::pair<string, object_map*> type_map_pair;

typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(ConsumerTable* consumer) :m_consumer(consumer)  { }
    ConsumerTable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef std::pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

typedef enum
{
    success,
    field_not_found,
    multiple_instances,
    not_resolved,
    failure
} ref_resolve_status;

typedef enum
{
    task_success,
    task_invalid_entry,
    task_failed,
    task_need_retry
} task_process_status;

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

    void dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);

protected:
    ConsumerMap m_consumerMap;

    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    ref_resolve_status resolveFieldRefValue(
        type_map                &type_maps,
        const string            &field_name,
        KeyOpFieldsValuesTuple  &tuple,
        sai_object_id_t         &sai_object);
    bool parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high);
    bool parseNameArray(const string &input, vector<string> &port_names);
    bool parseReference(type_map &type_maps, string &ref, string &table_name, string &object_name);
    bool tokenizeString(string str, const string &separator, vector<string> &tokens);
    ref_resolve_status resolveFieldRefArray(
        type_map                    &type_maps,
        const string                &field_name,
        KeyOpFieldsValuesTuple      &tuple,
        vector<sai_object_id_t>     &sai_object_arr);

private:
    DBConnector *m_db;
};

#endif /* SWSS_ORCH_H */
