#pragma once

#include <map>
#include <string>
#include <inttypes.h>

#include "orch.h"
#include "portsorch.h"

struct SflowPortInfo
{
    bool            admin_state;
    string          m_sample_dir;
    sai_object_id_t m_sample_id;
};

struct SflowSession
{
    sai_object_id_t m_sample_id;
    uint32_t        ref_count;
};

class SflowDropMonitor
{
public:
    SflowDropMonitor()
        : m_enable(false)
        , m_limitRate(0)
        , m_tamReport(SAI_NULL_OBJECT_ID)
        , m_tamEventAction(SAI_NULL_OBJECT_ID)
        , m_tamTransport(SAI_NULL_OBJECT_ID)
        , m_tamEvent(SAI_NULL_OBJECT_ID)
        , m_tamCollector(SAI_NULL_OBJECT_ID)
        , m_tam(SAI_NULL_OBJECT_ID)
        , m_policer(SAI_NULL_OBJECT_ID)
        , m_hostifTrapGroup(SAI_NULL_OBJECT_ID)
        , m_hostifUserDefinedTrap(SAI_NULL_OBJECT_ID)
    { }

    inline bool isEnabled() {
        return m_enable;
    }

    inline int32_t getLimitRate() {
        return m_limitRate;
    }

    bool enableDropMonitor(int32_t limit_rate);
    bool disableDropMonitor();

private:
    bool            m_enable;
    int32_t         m_limitRate; // packet per second

    sai_object_id_t m_tamReport;
    sai_object_id_t m_tamEventAction;
    sai_object_id_t m_tamTransport;
    sai_object_id_t m_tamEvent;
    sai_object_id_t m_tamCollector;
    sai_object_id_t m_tam;
    sai_object_id_t m_policer;
    sai_object_id_t m_hostifTrapGroup;
    sai_object_id_t m_hostifUserDefinedTrap;

    bool createTamReport();
    bool removeTamReport();

    bool createTamEventAction();
    bool removeTamEventAction();

    bool createTamTransport();
    bool removeTamTransport();

    bool createTamEvent();
    bool removeTamEvent();

    bool createTamCollector();
    bool removeTamCollector();

    bool createTam();
    bool removeTam();

    bool createPolicer(int32_t rate);
    bool removePolicer();

    bool createHostifTrapGroup();
    bool removeHostifTrapGroup();

    bool createHostifUserDefinedTrap();
    bool removeHostifUserDefinedTrap();

    bool initializeDropMonitor(int32_t limit_rate);
    void cleanupDropMonitor();
};

/* SAI Port to Sflow Port Info Map */
typedef std::map<sai_object_id_t, SflowPortInfo> SflowPortInfoMap;

/* Sample-rate(unsigned int) to Sflow session map */
typedef std::map<uint32_t, SflowSession> SflowRateSampleMap;

class SflowOrch : public Orch
{
public:
    SflowOrch(DBConnector* db, std::vector<std::string> &tableNames);

private:
    SflowPortInfoMap    m_sflowPortInfoMap;
    SflowRateSampleMap  m_sflowRateSampleMap;
    bool                m_sflowStatus;
    SflowDropMonitor    m_sflowDropMonitor;


    virtual void doTask(Consumer& consumer);
    bool sflowCreateSession(uint32_t rate, SflowSession &session);
    bool sflowDestroySession(SflowSession &session);
    bool sflowAddPort(sai_object_id_t sample_id, sai_object_id_t port_id, string direction);
    bool sflowDelPort(sai_object_id_t port_id, string direction);
    void sflowStatusSet(Consumer &consumer);
    bool sflowUpdateRate(sai_object_id_t port_id, uint32_t rate);
    bool sflowUpdateSampleDirection(sai_object_id_t port_id, string old_dir, string new_dir);
    uint32_t sflowSessionGetRate(sai_object_id_t sample_id);
    bool handleSflowSessionDel(sai_object_id_t port_id);
    void sflowExtractInfo(vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate, string &dir);
    void sflowExtractGlobalInfo(vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate, string &dir, int32_t &drop_monitor_limit);

};
