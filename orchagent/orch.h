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
#include "notificationconsumer.h"
#include "selectabletimer.h"

using namespace std;
using namespace swss;

const char delimiter           = ':';
const char list_item_delimiter = ',';
const char ref_start           = '[';
const char ref_end             = ']';
const char comma               = ',';
const char range_specifier     = '-';
const char config_db_key_delimiter = '|';

#define MLNX_PLATFORM_SUBSTRING "mellanox"
#define BRCM_PLATFORM_SUBSTRING "broadcom"
#define BFN_PLATFORM_SUBSTRING "barefoot"

#define CONFIGDB_KEY_SEPARATOR "|"
#define DEFAULT_KEY_SEPARATOR  ":"

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

class Orch;

// Design assumption
// 1. one Orch can have one or more Executor
// 2. one Executor must belong to one and only one Orch
// 3. Executor will hold an pointer to new-ed selectable, and delete it during dtor
class Executor : public Selectable
{
public:
    Executor(Selectable *selectable, Orch *orch)
        : m_selectable(selectable)
        , m_orch(orch)
    {
    }

    virtual ~Executor() { delete m_selectable; }

    // Decorating Selectable
    virtual void addFd(fd_set *fd) { return m_selectable->addFd(fd); }
    virtual bool isMe(fd_set *fd) { return m_selectable->isMe(fd); }
    virtual int readCache() { return m_selectable->readCache(); }
    virtual void readMe() { return m_selectable->readMe(); }

    // Disable copying
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Execute on event happening
    virtual void execute() { }
    virtual void drain() { }

protected:
    Selectable *m_selectable;
    Orch *m_orch;

    // Get the underlying selectable
    Selectable *getSelectable() const { return m_selectable; }
};

class Consumer : public Executor {
public:
    Consumer(TableConsumable *select, Orch *orch)
        : Executor(select, orch)
    {
    }

    TableConsumable *getConsumerTable() const
    {
        return static_cast<TableConsumable *>(getSelectable());
    }

    string getTableName() const
    {
        return getConsumerTable()->getTableName();
    }

    void execute();
    void drain();

    /* Store the latest 'golden' status */
    // TODO: hide?
    SyncMap m_toSync;
};

typedef map<string, std::shared_ptr<Executor>> ConsumerMap;

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
    Orch(DBConnector *db, const string tableName);
    Orch(DBConnector *db, const vector<string> &tableNames);
    Orch(const vector<TableConnector>& tables);
    virtual ~Orch();

    vector<Selectable*> getSelectables();

    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();

    /* Run doTask against a specific executor */
    virtual void doTask(Consumer &consumer) = 0;
    virtual void doTask(NotificationConsumer &consumer) { }
    virtual void doTask(SelectableTimer &timer) { }

    /* TODO: refactor recording */
    static void recordTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
protected:
    ConsumerMap m_consumerMap;

    static void logfileReopen();
    string dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    ref_resolve_status resolveFieldRefValue(type_map&, const string&, KeyOpFieldsValuesTuple&, sai_object_id_t&);
    bool parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high);
    bool parseReference(type_map &type_maps, string &ref, string &table_name, string &object_name);
    ref_resolve_status resolveFieldRefArray(type_map&, const string&, KeyOpFieldsValuesTuple&, vector<sai_object_id_t>&);

    /* Note: consumer will be owned by this class */
    void addExecutor(string executorName, Executor* executor);
private:
    void addConsumer(DBConnector *db, string tableName);
};

#endif /* SWSS_ORCH_H */
