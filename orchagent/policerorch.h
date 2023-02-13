#pragma once

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"

using namespace std;

typedef map<string, sai_object_id_t> PolicerTable;
typedef map<string, int> PolicerRefCountTable;

class PolicerOrch : public Orch
{
public:
    PolicerOrch(vector<TableConnector> &tableNames, PortsOrch *portOrch);

    bool policerExists(const string &name);
    bool getPolicerOid(const string &name, sai_object_id_t &oid);

    bool increaseRefCount(const string &name);
    bool decreaseRefCount(const string &name);
    task_process_status handlePortStormControlTable(swss::KeyOpFieldsValuesTuple tuple);
private:
    PortsOrch *m_portsOrch;
    virtual void doTask(Consumer& consumer);

    PolicerTable m_syncdPolicers;
    PolicerRefCountTable m_policerRefCounts;
};


