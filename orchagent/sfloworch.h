#pragma once

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"

using namespace std;

struct SflowSession
{
    bool            globalConfigured;
    bool            adminState;
    sai_object_id_t m_sample_id;
    uint32_t        rate;
};

/* SAI Port to Sflow session map */
typedef map<sai_object_id_t, SflowSession> SflowPortSessionMap;

/* Sample-rate(unsigned int) to SAI Samplesession object map */
typedef map<uint32_t, sai_object_id_t> SflowRateSampleMap;

/* SAI Samplesession object to reference(number of ports referencing this object) map*/
typedef map<sai_object_id_t, uint32_t> SflowSampleRefMap;

/* Speed to Samplerate map*/
typedef map<uint32_t, uint32_t> SflowSpeedRateMap;

class SflowOrch : public Orch
{
public:
    SflowOrch(DBConnector* db, vector<string> &tableNames);

private:
    SflowPortSessionMap m_sflowPortSessionMap;
    SflowRateSampleMap  m_sflowRateSampleMap;
    SflowSampleRefMap   m_sflowSampleRefMap;
    SflowSpeedRateMap   m_speedRateMap;
    bool                gEnable;
    bool                sflowStatus;

    virtual void doTask(Consumer& consumer);
    void doSflowStatusTask(Consumer &consumer);
    bool sflowCreateSession(SflowSession &session);
    bool sflowDestroySession(SflowSession &session);
    bool sflowAddPort(SflowSession &session_id, sai_object_id_t port_id);
    bool sflowDelPort(SflowSession &session_id, sai_object_id_t port_id);
    bool sflowStatusSet(bool enable, bool remove_session);
    bool handleSflowStatus(KeyOpFieldsValuesTuple tuple);
    bool handleGlobalConfig(KeyOpFieldsValuesTuple tuple);
    bool sflowGetDefaultSampleRate(Port port, uint32_t &rate);
    bool sflowGlobalConfigure(bool enable);
    bool sflowPortApplyGlobalSetting(Port port, SflowSession &session);
    bool sflowUpdateRate(sai_object_id_t port_id, uint32_t rate);
    void sflowUpdateSpeedRateMap(Consumer &consumer);
};
