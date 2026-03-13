#pragma once

#include <map>
#include <string>
#include <inttypes.h>

#include "orchagent/orch.h"
#include "orchagent/portsorch.h"

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
    void sflowExtractInfo(std::vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate, string &dir);
};
