#pragma once

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"

using namespace std;

struct SflowPortInfo
{
    bool            admin_state;
    sai_object_id_t m_sample_id;
};

struct SflowSession
{
    sai_object_id_t m_sample_id;
    uint32_t        ref_count;
};

/* SAI Port to Sflow Port Info Map */
typedef map<sai_object_id_t, SflowPortInfo> SflowPortInfoMap;

/* Sample-rate(unsigned int) to Sflow session map */
typedef map<uint32_t, SflowSession> SflowRateSampleMap;

class SflowOrch : public Orch
{
public:
    SflowOrch(DBConnector* db, vector<string> &tableNames);

private:
    SflowPortInfoMap    m_sflowPortInfoMap;
    SflowRateSampleMap  m_sflowRateSampleMap;
    bool                sflowStatus;

    virtual void doTask(Consumer& consumer);
    bool sflowCreateSession(uint32_t rate, SflowSession &session);
    bool sflowDestroySession(SflowSession &session);
    bool sflowAddPort(sai_object_id_t sample_id, sai_object_id_t port_id);
    bool sflowDelPort(sai_object_id_t port_id);
    void sflowStatusSet(Consumer &consumer);
    bool sflowUpdateRate(sai_object_id_t port_id, uint32_t rate);
    uint32_t sflowSessionGetRate(sai_object_id_t sample_id);
    bool handleSflowSessionDel(sai_object_id_t port_id);
    void sflowExtractFvs(vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate);
};
