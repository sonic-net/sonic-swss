#ifndef SWSS_ORCH_H
#define SWSS_ORCH_H

#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <memory>
#include <utility>
#include <condition_variable>

extern "C" {
#include <sai.h>
#include <saistatus.h>
}

#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"
#include "consumerstatetable.h"
#include "zmqconsumerstatetable.h"
#include "zmqserver.h"
#include "notificationconsumer.h"
#include "selectabletimer.h"
#include "macaddress.h"
#include "response_publisher.h"
#include "recorder.h"
#include "schema.h"

const char delimiter           = ':';
const char list_item_delimiter = ',';
const char ref_start           = '[';
const char ref_end             = ']';
const char comma               = ',';
const char range_specifier     = '-';
const char config_db_key_delimiter = '|';
const char state_db_key_delimiter  = '|';

#define MRVL_TL_PLATFORM_SUBSTRING "marvell-teralynx"
#define MRVL_PRST_PLATFORM_SUBSTRING "marvell-prestera"
#define MLNX_PLATFORM_SUBSTRING "mellanox"
#define BRCM_PLATFORM_SUBSTRING "broadcom"
#define BRCM_DNX_PLATFORM_SUBSTRING "broadcom-dnx"
#define BFN_PLATFORM_SUBSTRING  "barefoot"
#define VS_PLATFORM_SUBSTRING   "vs"
#define NPS_PLATFORM_SUBSTRING  "nephos"
#define CISCO_8000_PLATFORM_SUBSTRING "cisco-8000"
#define XS_PLATFORM_SUBSTRING   "xsight"

#define CONFIGDB_KEY_SEPARATOR "|"
#define DEFAULT_KEY_SEPARATOR  ":"
#define VLAN_SUB_INTERFACE_SEPARATOR "."

#define RING_SIZE 30
#define SLEEP_MSECONDS 500

const int default_orch_pri = 0;

typedef enum
{
    task_success,
    task_invalid_entry,
    task_failed,
    task_need_retry,
    task_ignore,
    task_duplicated
} task_process_status;

typedef struct
{
    // m_objsDependingOnMe stores names (without table name) of all objects depending on the current obj
    std::set<std::string> m_objsDependingOnMe;
    // m_objsReferencingByMe is a map from a field of the current object's to the object names it references
    // the object names are with table name
    // multiple objects being referenced are separated by ','
    std::map<std::string, std::string> m_objsReferencingByMe;
    sai_object_id_t m_saiObjectId;
    bool m_pendingRemove;
} referenced_object;

typedef std::map<std::string, referenced_object> object_reference_map;
typedef std::map<std::string, std::shared_ptr<object_reference_map>> type_map;

typedef std::map<std::string, sai_object_id_t> object_map;
typedef std::pair<std::string, sai_object_id_t> object_map_pair;

// Use multimap to support multiple OpFieldsValues for the same key (e,g, DEL and SET)
// The order of the key-value pairs whose keys compare equivalent is the order of
// insertion and does not change. (since C++11)
typedef std::multimap<std::string, swss::KeyOpFieldsValuesTuple> SyncMap;

typedef std::pair<std::string, int> table_name_with_pri_t;

class Orch;

using AnyTask = std::function<void()>; // represents a function with no argument and returns void

class RingBuffer;

// Design assumption
// 1. one Orch can have one or more Executor
// 2. one Executor must belong to one and only one Orch
// 3. Executor will hold an pointer to new-ed selectable, and delete it during dtor
class Executor : public swss::Selectable
{
public:
    Executor(swss::Selectable *selectable, Orch *orch, const std::string &name)
        : m_selectable(selectable)
        , m_orch(orch)
        , m_name(name)
    {
    }

    virtual ~Executor() { delete m_selectable; }

    // Decorating Selectable
    int getFd() override { return m_selectable->getFd(); }
    uint64_t readData() override { return m_selectable->readData(); }
    bool hasCachedData() override { return m_selectable->hasCachedData(); }
    bool initializedWithData() override { return m_selectable->initializedWithData(); }
    void updateAfterRead() override { m_selectable->updateAfterRead(); }

    // Disable copying
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // Execute on event happening
    virtual void execute() { }
    virtual void drain() { }

    virtual std::string getName() const
    {
        return m_name;
    }

    Orch *getOrch() const { return m_orch; }
    static std::shared_ptr<RingBuffer> gRingBuffer;
    void processAnyTask(AnyTask&& func);

protected:
    swss::Selectable *m_selectable;
    Orch *m_orch;

    // Name for Executor
    std::string m_name;

    // Get the underlying selectable
    swss::Selectable *getSelectable() const { return m_selectable; }
};

typedef std::map<std::string, std::shared_ptr<Executor>> ConsumerMap;

class ConsumerBase : public Executor {
public:
    ConsumerBase(swss::Selectable *selectable, Orch *orch, const std::string &name)
        : Executor(selectable, orch, name)
    {
    }

    virtual swss::TableBase *getConsumerTable() const = 0;

    std::string getTableName() const
    {
        return getConsumerTable()->getTableName();
    }

    std::string dumpTuple(const swss::KeyOpFieldsValuesTuple &tuple);
    void dumpPendingTasks(std::vector<std::string> &ts);

    /* Store the latest 'golden' status */
    // TODO: hide?
    SyncMap m_toSync;

    /* record the tuple */
    void recordTuple(const swss::KeyOpFieldsValuesTuple &tuple);

    void addToSync(const swss::KeyOpFieldsValuesTuple &entry);

    // Returns: the number of entries added to m_toSync
    size_t addToSync(const std::deque<swss::KeyOpFieldsValuesTuple> &entries);
    size_t addToSync(std::shared_ptr<std::deque<swss::KeyOpFieldsValuesTuple>> entries);

    size_t refillToSync();
    size_t refillToSync(swss::Table* table);
};

class RingBuffer
{
private:
    std::vector<AnyTask> buffer;
    int head = 0;
    int tail = 0;
    std::set<std::string> m_consumerSet;

    std::condition_variable cv;
    std::mutex mtx;
    bool idle_status = true;

public:
    RingBuffer(int size=RING_SIZE);
    bool thread_created = false;
    std::atomic<bool> thread_exited{false};

    // pause the ring thread if the buffer is empty
    void pauseThread();
    // wake up the ring thread in case it's locked but not empty
    void notify();

    bool IsFull() const;
    bool IsEmpty() const;
    bool IsIdle() const;

    bool push(AnyTask entry);
    bool pop(AnyTask& entry);

    void addExecutor(Executor* executor);
    bool serves(const std::string& tableName);
    void setIdle(bool idle);
};

class Consumer : public ConsumerBase {
public:
    Consumer(swss::ConsumerTableBase *select, Orch *orch, const std::string &name)
        : ConsumerBase(select, orch, name)
    {
    }

    swss::ConsumerTableBase *getConsumerTable() const override
    {
        // ConsumerTableBase is a subclass of TableBase
        return static_cast<swss::ConsumerTableBase *>(getSelectable());
    }

    const swss::DBConnector* getDbConnector() const
    {
        auto table = static_cast<swss::ConsumerTableBase *>(getSelectable());
        return table->getDbConnector();
    }

    int getDbId() const
    {
        return getDbConnector()->getDbId();
    }

    std::string getDbName() const
    {
        return getDbConnector()->getDbName();
    }

    void execute() override;
    void drain() override;
};

typedef enum
{
    success,
    field_not_found,
    multiple_instances,
    not_resolved,
    empty,
    failure
} ref_resolve_status;

typedef std::pair<swss::DBConnector *, std::string> TableConnector;
typedef std::pair<swss::DBConnector *, std::vector<std::string>> TablesConnector;

class Orch
{
public:
    Orch(swss::DBConnector *db, const std::string tableName, int pri = default_orch_pri);
    Orch(swss::DBConnector *db, const std::vector<std::string> &tableNames);
    Orch(swss::DBConnector *db1, swss::DBConnector *db2,
        const std::vector<std::string> &tableNames_1, const std::vector<std::string> &tableNames_2);
    Orch(swss::DBConnector *db, const std::vector<table_name_with_pri_t> &tableNameWithPri);
    Orch(const std::vector<TableConnector>& tables);
    virtual ~Orch() = default;

    static std::shared_ptr<RingBuffer> gRingBuffer;

    std::vector<swss::Selectable*> getSelectables();

    // add the existing table data (left by warm reboot) to the consumer todo task list.
    size_t addExistingData(swss::Table *table);
    size_t addExistingData(const std::string& tableName);

    // Prepare for warm start if Redis contains valid input data
    // otherwise fallback to cold start
    virtual bool bake();

    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    virtual void doTask();

    /* Run doTask against a specific executor */
    virtual void doTask(Consumer &consumer) { };
    virtual void doTask(swss::NotificationConsumer &consumer) { }
    virtual void doTask(swss::SelectableTimer &timer) { }

    void dumpPendingTasks(std::vector<std::string> &ts);

    /**
     * @brief Flush pending responses
     */
    void flushResponses();
protected:
    ConsumerMap m_consumerMap;

    Orch();
    ref_resolve_status resolveFieldRefValue(type_map&, const std::string&, const std::string&, swss::KeyOpFieldsValuesTuple&, sai_object_id_t&, std::string&);
    std::set<std::string> generateIdListFromMap(unsigned long idsMap, sai_uint32_t maxId);
    unsigned long generateBitMapFromIdsStr(const std::string &idsStr);
    bool isItemIdsMapContinuous(unsigned long idsMap, sai_uint32_t maxId);
    bool parseIndexRange(const std::string &input, sai_uint32_t &range_low, sai_uint32_t &range_high);
    bool parseReference(type_map &type_maps, std::string &ref, const std::string &table_name, std::string &object_name);
    ref_resolve_status resolveFieldRefArray(type_map&, const std::string&, const std::string&, swss::KeyOpFieldsValuesTuple&, std::vector<sai_object_id_t>&, std::string&);
    void setObjectReference(type_map&, const std::string&, const std::string&, const std::string&, const std::string&);
    bool doesObjectExist(type_map&, const std::string&, const std::string&, const std::string&, std::string&);
    void removeObject(type_map&, const std::string&, const std::string&);
    bool isObjectBeingReferenced(type_map&, const std::string&, const std::string&);
    std::string objectReferenceInfo(type_map&, const std::string&, const std::string&);
    void removeMeFromObjsReferencedByMe(type_map &type_maps, const std::string &table, const std::string &obj_name, const std::string &field, const std::string &old_referenced_obj_name, bool remove_field=true);

    /* Note: consumer will be owned by this class */
    void addExecutor(Executor* executor);
    Executor *getExecutor(std::string executorName);

    ResponsePublisher m_publisher{"APPL_STATE_DB"};
private:
    void addConsumer(swss::DBConnector *db, std::string tableName, int pri = default_orch_pri);
};

#include "request_parser.h"

class Orch2 : public Orch
{
public:
    Orch2(swss::DBConnector *db, const std::string& tableName, Request& request, int pri=default_orch_pri)
        : Orch(db, tableName, pri), request_(request)
    {
    }

    Orch2(swss::DBConnector *db, const std::vector<std::string> &tableNames, Request& request)
        : Orch(db, tableNames), request_(request)
    {
    }

protected:
    virtual void doTask(Consumer& consumer);

    virtual bool addOperation(const Request& request)=0;
    virtual bool delOperation(const Request& request)=0;

private:
    Request& request_;
};

#endif /* SWSS_ORCH_H */
