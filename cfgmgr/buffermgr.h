#ifndef __BUFFMGR__
#define __BUFFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>

namespace swss {

#define INGRESS_LOSSLESS_PG_POOL_NAME "ingress_lossless_pool"
#define LOSSLESS_PGS "3-4"

typedef struct{
    string size;
    string xon;
    string xon_offset;
    string xoff;
    string threshold;
} pg_profile_t;

typedef map<string, pg_profile_t> speed_map_t;
typedef map<string, speed_map_t> pg_profile_lookup_t;

typedef map<string, string> port_cable_length_t;

class BufferMgr : public Orch
{
public:
    BufferMgr(DBConnector *cfgDb, DBConnector *stateDb, string pg_lookup_file, const vector<string> &tableNames);
    using Orch::doTask;

private:
    Table m_statePortTable;
    Table m_cfgPortTable;
    Table m_cfgCableLenTable;
    Table m_cfgBufferProfileTable;
    Table m_cfgBufferPgTable;
    Table m_cfgLosslessPgPoolTable;

    pg_profile_lookup_t m_pgProfileLookup;
    port_cable_length_t m_cableLenLookup;
    std::string getPgPoolMode();
    void readPgProfileLookupFile(std::string);
    task_process_status doCableTask(string port, string cable_length);
    task_process_status doSpeedUpdateTask(string port, string speed);

    void doTask(Consumer &consumer);
};

}

#endif /* __BUFFMGR__ */
