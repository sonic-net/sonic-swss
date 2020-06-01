#pragma once

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"
#include "observer.h"

using namespace std;

typedef enum 
{
    POLICER_TYPE_NAMED = 0,
    POLICER_TYPE_STORM_CONTROL,
    POLICER_TYPE_MAX
} policer_type_t;

struct PolicerEntry
{
    string name;
    policer_type_t type;
    sai_object_id_t policerOid;
};

typedef map<string, PolicerEntry> PolicerTable;
typedef map<string, int> PolicerRefCountTable;

class PolicerOrch : public Orch, public Observer
{
public:
    PolicerOrch(vector<TableConnector> &tableNames, PortsOrch *portOrch);

    bool policerExists(const string &name);
    bool getPolicerOid(const string &name, sai_object_id_t &oid);

    bool increaseRefCount(const string &name);
    bool decreaseRefCount(const string &name);
    task_process_status handlePortStormControlTable(Consumer& consumer);
    task_process_status handlePolicerTable(Consumer& consumer);
    void update(SubjectType, void *);
    bool handlePhyDelete(Port &port);

private:
    PortsOrch *m_portsOrch;
    virtual void doTask(Consumer& consumer);

    PolicerTable m_syncdPolicers;
    PolicerRefCountTable m_policerRefCounts;

    typedef task_process_status (PolicerOrch::*policer_type_table_handler)(Consumer& consumer);
    typedef map<string, policer_type_table_handler> policer_type_table_handler_map;
    typedef pair<string, policer_type_table_handler> policer_type_table_handler_pair;
    policer_type_table_handler_map m_policer_type_table_handler_map;
    void initPolicerTypeTableHandlers();

    bool isStormControlPolicer(string policer_name);
};
