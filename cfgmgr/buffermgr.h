#ifndef __BUFFMGR__
#define __BUFFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>

namespace swss {

#define INGRESS_LOSSLESS_PG_POOL_NAME "ingress_lossless_pool"

#define BUFFERMGR_TIMER_PERIOD 10

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
typedef std::map<std::string, std::string> port_speed_t;
typedef std::map<std::string, std::string> port_pfc_status_t;
typedef std::map<std::string, std::string> port_admin_status_t;

class BufferMgr : public Orch
{
public:
    BufferMgr(DBConnector *cfgDb, DBConnector *applDb, std::string pg_lookup_file, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    std::string m_platform;

    Table m_cfgPortTable;
    Table m_cfgCableLenTable;
    Table m_cfgBufferProfileTable;
    Table m_cfgBufferPgTable;
    Table m_cfgLosslessPgPoolTable;

    ProducerStateTable m_applBufferProfileTable;
    ProducerStateTable m_applBufferPgTable;
    ProducerStateTable m_applBufferPoolTable;
    ProducerStateTable m_applBufferQueueTable;
    ProducerStateTable m_applBufferIngressProfileListTable;
    ProducerStateTable m_applBufferEgressProfileListTable;

    bool m_pgfile_processed;
    bool dynamic_buffer_model;
    /*
     * True if cable lengths and speeds are predetermined. If it is true, we
     * do not dynamically generate BUFFER_PROFILE and BUFFER_PG entries in
     * APPL_DB (i.e., we just copy static entries from CONFIG_DB).
     */
    bool fixed_cable_speed_len;

    pg_profile_lookup_t m_pgProfileLookup;
    port_cable_length_t m_cableLenLookup;
    port_admin_status_t m_portStatusLookup;
    port_speed_t m_speedLookup;
    std::string getPgPoolMode();
    void readPgProfileLookupFile(std::string);
    task_process_status doCableTask(std::string port, std::string cable_length);
    task_process_status doSpeedUpdateTask(std::string port);
    void doBufferTableTask(Consumer &consumer, ProducerStateTable &applTable);

    void transformSeperator(std::string &name);

    void doTask(Consumer &consumer);
    void doBufferMetaTask(Consumer &consumer);
    
    port_pfc_status_t m_portPfcStatus;
    void doPortQosTableTask(Consumer &consumer);

};

}

#endif /* __BUFFMGR__ */
