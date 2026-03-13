#include "sai.h"
#include "orchagent/sfloworch.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern sai_samplepacket_api_t* sai_samplepacket_api;
extern sai_port_api_t*         sai_port_api;
extern sai_object_id_t         gSwitchId;
extern PortsOrch*              gPortsOrch;

SflowOrch::SflowOrch(DBConnector* db, vector<string> &tableNames) :
    Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
    m_sflowStatus = false;
}

bool SflowOrch::sflowCreateSession(uint32_t rate, SflowSession &session)
{
    sai_attribute_t attr;
    sai_object_id_t session_id = SAI_NULL_OBJECT_ID;
    sai_status_t    sai_rc;

    attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    attr.value.u32 = rate;

    sai_rc = sai_samplepacket_api->create_samplepacket(&session_id, gSwitchId,
                                                       1, &attr);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create sample packet session with rate %d", rate);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_SAMPLEPACKET, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    session.m_sample_id = session_id;
    session.ref_count = 0;
    return true;
}

bool SflowOrch::sflowDestroySession(SflowSession &session)
{
    sai_status_t    sai_rc;

    sai_rc = sai_samplepacket_api->remove_samplepacket(session.m_sample_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to destroy sample packet session with id %" PRIx64 "",
                       session.m_sample_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_SAMPLEPACKET, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool SflowOrch::sflowUpdateRate(sai_object_id_t port_id, uint32_t rate)
{
    auto         port_info = m_sflowPortInfoMap.find(port_id);
    auto         session = m_sflowRateSampleMap.find(rate);
    SflowSession new_session;
    uint32_t     old_rate = sflowSessionGetRate(port_info->second.m_sample_id);

    if (session == m_sflowRateSampleMap.end())
    {
        if (!sflowCreateSession(rate, new_session))
        {
            SWSS_LOG_ERROR("Creating sflow session with rate %d failed", rate);
            return false;
        }
        m_sflowRateSampleMap[rate] = new_session;
    }
    else
    {
        new_session = session->second;
    }

    if (port_info->second.admin_state)
    {
        if (!sflowAddPort(new_session.m_sample_id, port_id, port_info->second.m_sample_dir))
        {
            return false;
        }
    }
    port_info->second.m_sample_id = new_session.m_sample_id;

    m_sflowRateSampleMap[rate].ref_count++;
    m_sflowRateSampleMap[old_rate].ref_count--;
    if (m_sflowRateSampleMap[old_rate].ref_count == 0)
    {
        if (!sflowDestroySession(m_sflowRateSampleMap[old_rate]))
        {
            SWSS_LOG_ERROR("Failed to clean old session %" PRIx64 " with rate %d",
                           m_sflowRateSampleMap[old_rate].m_sample_id, old_rate);
        }
        else
        {
            m_sflowRateSampleMap.erase(old_rate);
        }
    }
    return true;
}

bool SflowOrch::sflowAddPort(sai_object_id_t sample_id, sai_object_id_t port_id, string direction)
{
    sai_attribute_t attr;
    sai_status_t    sai_rc;

    SWSS_LOG_DEBUG("sflowAddPort  %" PRIx64 " portOid %" PRIx64 " dir %s",
                           sample_id, port_id, direction.c_str());

    if (direction == "both" || direction == "rx")
    {
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = sample_id;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set session %" PRIx64 " on port %" PRIx64, sample_id, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    if (direction == "both" || direction == "tx")
    {
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = sample_id;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set session %" PRIx64 " on port %" PRIx64, sample_id, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    return true;
}

bool SflowOrch::sflowDelPort(sai_object_id_t port_id, string direction)
{
    sai_attribute_t attr;
    sai_status_t    sai_rc;

    SWSS_LOG_DEBUG("sflowDelPort  portOid %" PRIx64 " dir %s",
                           port_id, direction.c_str());

    if (direction == "both" || direction == "rx")
    {
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete session on port %" PRIx64, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    if (direction == "both" || direction == "tx")
    {
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete session on port %" PRIx64, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    return true;
}

bool SflowOrch::sflowUpdateSampleDirection(sai_object_id_t port_id, string old_dir, string new_dir)
{
    sai_object_id_t ing_sample_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t egr_sample_oid = SAI_NULL_OBJECT_ID;
    sai_attribute_t attr;
    sai_status_t    sai_rc;
    auto            port_info = m_sflowPortInfoMap.find(port_id);

    SWSS_LOG_DEBUG("sflowUpdateSampleDirection  portOid %" PRIx64 " old dir %s new dir %s",
                           port_id, old_dir.c_str(), new_dir.c_str());

    if ((new_dir == "tx") && (old_dir == "rx" || old_dir == "both"))
    {
        ing_sample_oid = SAI_NULL_OBJECT_ID;
        egr_sample_oid = port_info->second.m_sample_id;
    }

    if ((new_dir == "rx") && (old_dir == "tx" || old_dir == "both"))
    {
        ing_sample_oid = port_info->second.m_sample_id;
        egr_sample_oid = SAI_NULL_OBJECT_ID;
    }

    if ((new_dir == "both") && (old_dir == "tx" || old_dir == "rx"))
    {
        ing_sample_oid = port_info->second.m_sample_id;
        egr_sample_oid = port_info->second.m_sample_id;
    }

    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = ing_sample_oid;
    sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to Ingress session on port %" PRIx64, port_id);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = egr_sample_oid;
    sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to Update Egress session on port %" PRIx64, port_id);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

void SflowOrch::sflowExtractInfo(vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate, string &dir)
{
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_state")
        {
            if (fvValue(i) == "up")
            {
                admin = true;
            }
            else if (fvValue(i) == "down")
            {
                admin = false;
            }
        }
        else if (fvField(i) == "sample_rate")
        {
            if (fvValue(i) != "error")
            {
                rate = (uint32_t)stoul(fvValue(i));
            }
            else
            {
                rate = 0;
            }
        }
        else if (fvField(i) == "sample_direction")
        {
            if (fvValue(i) != "error")
            {
                dir = fvValue(i);
            }
        }
    }
}

void SflowOrch::sflowStatusSet(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string op = kfvOp(tuple);
        uint32_t rate = 0;
        string dir = "";

        if (op == SET_COMMAND)
        {
            sflowExtractInfo(kfvFieldsValues(tuple), m_sflowStatus, rate, dir);
        }
        else if (op == DEL_COMMAND)
        {
            m_sflowStatus = false;
        }
        it = consumer.m_toSync.erase(it);
    }
}

uint32_t SflowOrch::sflowSessionGetRate(sai_object_id_t m_sample_id)
{
    for (auto it: m_sflowRateSampleMap)
    {
        if (it.second.m_sample_id == m_sample_id)
        {
            return it.first;
        }
    }
    return 0;
}

bool SflowOrch::handleSflowSessionDel(sai_object_id_t port_id)
{
    auto sflowInfo = m_sflowPortInfoMap.find(port_id);

    if (sflowInfo != m_sflowPortInfoMap.end())
    {
        uint32_t rate = sflowSessionGetRate(sflowInfo->second.m_sample_id);
        if (sflowInfo->second.admin_state)
        {
            if (!sflowDelPort(port_id, sflowInfo->second.m_sample_dir))
            {
                return false;
            }
            sflowInfo->second.admin_state = false;
        }

        m_sflowPortInfoMap.erase(port_id);
        m_sflowRateSampleMap[rate].ref_count--;
        if (m_sflowRateSampleMap[rate].ref_count == 0)
        {
            if (!sflowDestroySession(m_sflowRateSampleMap[rate]))
            {
                return false;
            }
            m_sflowRateSampleMap.erase(rate);
        }
    }
    return true;
}

void SflowOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    Port   port;
    string table_name = consumer.getTableName();

    if (table_name == APP_SFLOW_TABLE_NAME)
    {
        sflowStatusSet(consumer);
        return;
    }
    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string op = kfvOp(tuple);
        string alias = kfvKey(tuple);

        gPortsOrch->getPort(alias, port);
        if (op == SET_COMMAND)
        {
            bool      admin_state = m_sflowStatus;
            uint32_t  rate       = 0;
            string dir = "rx";

            if (!m_sflowStatus)
            {
                return;
            }
            auto sflowInfo = m_sflowPortInfoMap.find(port.m_port_id);
            if (sflowInfo != m_sflowPortInfoMap.end())
            {
                rate = sflowSessionGetRate(sflowInfo->second.m_sample_id);
                admin_state = sflowInfo->second.admin_state;
            }

            SWSS_LOG_DEBUG(" Existing Cfg portOid %" PRIx64 " admin %d rate %d dir %s", 
                            port.m_port_id, (unsigned int)admin_state, rate, 
                            sflowInfo->second.m_sample_dir.c_str());

            sflowExtractInfo(kfvFieldsValues(tuple), admin_state, rate, dir);

            SWSS_LOG_DEBUG("New Cfg  portOid %" PRIx64 " admin %d rate %d dir %s", 
                            port.m_port_id, (unsigned int)admin_state, rate, dir.c_str());

            if (sflowInfo == m_sflowPortInfoMap.end())
            {
                if (rate == 0)
                {
                    it++;
                    continue;
                }

                SflowPortInfo port_info;
                auto          session_info = m_sflowRateSampleMap.find(rate);
                if (session_info != m_sflowRateSampleMap.end())
                {
                    port_info.m_sample_id = session_info->second.m_sample_id;
                }
                else
                {
                    SflowSession  session;
                    if (!sflowCreateSession(rate, session))
                    {
                        it++;
                        continue;
                    }
                    m_sflowRateSampleMap[rate] = session;
                    port_info.m_sample_id = session.m_sample_id;
                }
                port_info.m_sample_dir = dir;

                if (admin_state)
                {
                    if (!sflowAddPort(port_info.m_sample_id, port.m_port_id, port_info.m_sample_dir))
                    {
                        it++;
                        continue;
                    }
                }
                port_info.admin_state = admin_state;
                m_sflowPortInfoMap[port.m_port_id] = port_info;
                m_sflowRateSampleMap[rate].ref_count++;
            }
            else
            {
                if (rate != sflowSessionGetRate(sflowInfo->second.m_sample_id))
                {
                    if (!sflowUpdateRate(port.m_port_id, rate))
                    {
                        it++;
                        continue;
                    }
                }
                if (admin_state != sflowInfo->second.admin_state)
                {
                    bool ret = false;
                    if (admin_state)
                    {
                        ret = sflowAddPort(sflowInfo->second.m_sample_id, port.m_port_id,
                                            sflowInfo->second.m_sample_dir);
                    }
                    else
                    {
                        ret = sflowDelPort(port.m_port_id, sflowInfo->second.m_sample_dir);
                    }
                    if (!ret)
                    {
                        it++;
                        continue;
                    }
                    sflowInfo->second.admin_state = admin_state;
                }

                if (dir !=  sflowInfo->second.m_sample_dir)
                {
                    string old_dir = sflowInfo->second.m_sample_dir;
                    if (!sflowUpdateSampleDirection(port.m_port_id, old_dir, dir))
                    {
                        it++;
                        continue;
                    }
                    sflowInfo->second.m_sample_dir = dir;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!handleSflowSessionDel(port.m_port_id))
            {
                it++;
                continue;
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}
