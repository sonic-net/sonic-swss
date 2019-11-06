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
    std::string size;
    std::string xon;
    std::string xon_offset;
    std::string xoff;
    std::string threshold;
} pg_profile_t;

typedef std::map<std::string, pg_profile_t> speed_map_t;
typedef std::map<std::string, speed_map_t> pg_profile_lookup_t;

typedef std::map<std::string, std::string> port_cable_length_t;

class BufferMgr : public Orch
{
public:
    BufferMgr(DBConnector *cfgDb, DBConnector *stateDb, std::string pg_lookup_file, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    Table m_cfgPortTable;
    Table m_cfgCableLenTable;
    Table m_cfgBufferProfileTable;
    Table m_cfgBufferPgTable;
    Table m_cfgLosslessPgPoolTable;
    bool m_pgfile_processed;

    pg_profile_lookup_t m_pgProfileLookup;
    port_cable_length_t m_cableLenLookup;
    std::string getPgPoolMode();
    void readPgProfileLookupFile(std::string);
    task_process_status doCableTask(std::string port, std::string cable_length);
    task_process_status doSpeedUpdateTask(std::string port, std::string speed);

    void doTask(Consumer &consumer);
};

}

#endif /* __BUFFMGR__ */
