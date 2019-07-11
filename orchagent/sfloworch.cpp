#include "sai.h"
#include "sfloworch.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern sai_samplepacket_api_t*   sai_samplepacket_api;
extern sai_port_api_t*   sai_port_api;

extern sai_object_id_t gSwitchId;
extern PortsOrch* gPortsOrch;


bool SflowOrch::sflowGetDefaultSampleRate(Port port, uint32_t &rate)
{
    string speed_str = to_string(port.m_speed);
    string rate_str;
    bool ret = m_sflowSampleRateTable->hget("global", speed_str, rate_str);


    if(ret == false) 
    {
        SWSS_LOG_ERROR("Unable to find default rate for speed %d", port.m_speed);
        return false;
    }

    rate = (uint32_t)stoul(rate_str);
    return true;
}

SflowOrch::SflowOrch(DBConnector* db, vector<string> &tableNames) :
    Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
    m_sflowSampleRateTable = unique_ptr<Table>(new Table(db, APP_SFLOW_SAMPLE_RATE_TABLE_NAME));
    gEnable = true;
    sflowStatus = false;
}

bool SflowOrch::sflowCreateSession(SflowSession &session)
{
    sai_attribute_t attr;
    sai_object_id_t session_id = SAI_NULL_OBJECT_ID;
    sai_status_t    sai_rc;

    attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    attr.value.u32 = session.rate;

    sai_rc = sai_samplepacket_api->create_samplepacket(&session_id, gSwitchId,
                                                       1, &attr);
    if(sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create sample packet session with rate %d",
                       session.rate);
        return false;
    }

    session.m_sample_id = session_id;
    return true;
}

bool SflowOrch::sflowDestroySession(SflowSession &session)
{
    sai_status_t    sai_rc;

    sai_rc = sai_samplepacket_api->remove_samplepacket(session.m_sample_id);
    if(sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to destroy sample packet session with id %lx",
                       session.m_sample_id);
        return false;
    }

    return true;
}

bool SflowOrch::sflowUpdateRate(sai_object_id_t port_id, uint32_t rate)
{
    auto old_session = m_sflowPortSessionMap.find(port_id);
    auto sample_obj = m_sflowRateSampleMap.find(rate);
    SflowSession new_session;

    if (sample_obj ==  m_sflowRateSampleMap.end())
    {
        new_session.rate = rate;
        if (!sflowCreateSession(new_session))
        {
            SWSS_LOG_ERROR("Creating sflow session with rate %d failed", rate);
            return false;
        }
        m_sflowRateSampleMap[rate] = new_session.m_sample_id;
        m_sflowSampleRefMap[new_session.m_sample_id] = 0;
    }
    else
    {
        new_session.m_sample_id = sample_obj->second;
    }

    if (old_session->second.adminState)
    {
        if (!sflowAddPort(new_session, port_id))
        {
            return false;
        }
    }

    m_sflowSampleRefMap[new_session.m_sample_id]++;
    m_sflowSampleRefMap[old_session->second.m_sample_id]--;

    if (m_sflowSampleRefMap[old_session->second.m_sample_id] == 0)
    {
        if (!sflowDestroySession(old_session->second))
        {
            SWSS_LOG_ERROR("Failed to clean old session %lx",
                           old_session->second.m_sample_id);
        }
        else
        {
            m_sflowRateSampleMap.erase(old_session->second.rate);
            m_sflowSampleRefMap.erase(old_session->second.m_sample_id);
        }
    }
    new_session.globalConfigured = old_session->second.globalConfigured;
    new_session.adminState = old_session->second.adminState;

    m_sflowPortSessionMap[port_id] = new_session;
    return true;
}

bool SflowOrch::sflowAddPort(SflowSession &session, sai_object_id_t port_id)
{
    sai_attribute_t attr;
    sai_status_t    sai_rc;

    if (!sflowStatus)
    {
        return true;
    }

    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = session.m_sample_id;

    sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set session %lx on port %lx",
                       session.m_sample_id, port_id);
        return false;
    }
    return true;
}

bool SflowOrch::sflowDelPort(SflowSession &session, sai_object_id_t port_id)
{
    sai_attribute_t attr;
    sai_status_t    sai_rc;


    if (!sflowStatus)
    {
        return true;
    }

    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = SAI_NULL_OBJECT_ID;

    sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

    if(sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete session %lx on port %lx",
                       session.m_sample_id, port_id);
        return false;
    }
    return true;
}

bool SflowOrch::sflowGlobalConfigure(bool enable)
{
    bool ret = true;

    for(auto& pair: gPortsOrch->getAllPorts())
    {
        auto& port = pair.second;
        if (port.m_type != Port::PHY) continue;

        auto sflowInfo = m_sflowPortSessionMap.find(port.m_port_id);
        if(sflowInfo == m_sflowPortSessionMap.end())
        {
            if(!enable)
            {
                continue;
            }
            SflowSession session;
            uint32_t     rate = 0;

            sflowGetDefaultSampleRate(port, rate);
            session.rate = rate;
            session.globalConfigured = true;
            session.adminState = true;
            auto e_sampleObj = m_sflowRateSampleMap.find(rate);

            if (e_sampleObj != m_sflowRateSampleMap.end())
            {
                session.m_sample_id = e_sampleObj->second;
            }
            else 
            {
                if (!sflowCreateSession(session))
                {
                    SWSS_LOG_NOTICE("Creating sflow session with rate %d failed", rate);
                    ret = false;
                    continue;
                }
                m_sflowRateSampleMap[rate] = session.m_sample_id;
                m_sflowSampleRefMap[session.m_sample_id] = 0;
            }

            if (sflowAddPort(session, port.m_port_id))
            {
                m_sflowPortSessionMap[port.m_port_id] = session;
                m_sflowSampleRefMap[session.m_sample_id]++;
            }
            else
            {
                SWSS_LOG_ERROR("Failed to add port %s to global session",
                               port.m_alias.c_str());
                ret = false;

                if (m_sflowSampleRefMap[session.m_sample_id] == 0)
                {
                    if (sflowDestroySession(session))
                    {
                        m_sflowSampleRefMap.erase(session.m_sample_id);
                        m_sflowRateSampleMap.erase(rate);
                    }
                }
            }
        }
        else
        {
            if(!sflowInfo->second.globalConfigured)
            {
                continue;
            }
            if(sflowInfo->second.adminState != enable)
            {
                if(!enable)
                {
                    if (sflowDelPort(sflowInfo->second, port.m_port_id))
                    {
                        sflowInfo->second.adminState = enable;
                    }
                    else 
                    {
                        SWSS_LOG_ERROR("Failed to disable global sflow on port %s",
                                       port.m_alias.c_str());
                        ret = false;
                    }
                }
                else 
                {
                    if (sflowAddPort(sflowInfo->second, port.m_port_id))
                    {
                        sflowInfo->second.adminState = enable;
                    }
                    else 
                    {
                        SWSS_LOG_ERROR("Failed to enable global sflow on port %s",
                                       port.m_alias.c_str());
                        ret = false;
                    }
                }
            }
        }
    }
    return ret;
}

bool SflowOrch::sflowStatusSet(bool enable, bool remove_session)
{
    bool            ret = true;
    sai_attribute_t attr;
    sai_status_t    sai_rc;

    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;

    if (enable)
    {
        if (!sflowGlobalConfigure(gEnable))
        {
            ret = false;
        }
    }

    for (auto& pair: gPortsOrch->getAllPorts())
    {
        auto& port = pair.second;
        if (port.m_type != Port::PHY) continue;

        auto sflowInfo = m_sflowPortSessionMap.find(port.m_port_id);
        if (sflowInfo != m_sflowPortSessionMap.end())
        {
            if (sflowInfo->second.adminState)
            {
                if (!enable)
                {
                    attr.value.oid = SAI_NULL_OBJECT_ID;
                }
                else
                {
                    attr.value.oid = sflowInfo->second.m_sample_id;
                }

                sai_rc = sai_port_api->set_port_attribute(port.m_port_id, &attr);

                if (sai_rc != SAI_STATUS_SUCCESS)
                {
                    ret = false;
                    SWSS_LOG_ERROR("Failed to re-configure sflow on port %s",
                                    port.m_alias.c_str());
                }
            }

            if (remove_session)
            {
                m_sflowSampleRefMap[sflowInfo->second.m_sample_id]--;
                if (m_sflowSampleRefMap[sflowInfo->second.m_sample_id] == 0)
                {
                    if (sflowDestroySession(sflowInfo->second))
                    {
                        m_sflowSampleRefMap.erase(sflowInfo->second.m_sample_id);
                        m_sflowRateSampleMap.erase(sflowInfo->second.rate);
                    }
                }
                m_sflowPortSessionMap.erase(port.m_port_id);
            }
        }
    }
    return ret;
}

bool SflowOrch::sflowPortApplyGlobalSetting(Port port, SflowSession &session)
{
    uint32_t     rate = 0;

    sflowGetDefaultSampleRate(port, rate);

    if (rate != session.rate)
    {
        if (!sflowUpdateRate(port.m_port_id, rate))
        {
            return false;
        }
    }

    if(session.adminState != gEnable)
    {
        if (gEnable)
        {
            if(!sflowAddPort(session, port.m_port_id))
            {
                SWSS_LOG_ERROR("Updating port with session %lx failed", session.m_sample_id);
                return false;
            }
        }
        else
        {
            if(!sflowDelPort(session, port.m_port_id))
            {
                SWSS_LOG_ERROR("Updating port with session %lx failed", session.m_sample_id);
                return false;
            }

        }
    }
    session.adminState = gEnable;
    session.globalConfigured = true;

    return true;
}

bool SflowOrch::handleSflowStatus(KeyOpFieldsValuesTuple tuple)
{
    bool ret = true;
    string op = kfvOp(tuple);

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "admin_state")
            {
                if (fvValue(i) == "enable")
                {
                    if(sflowStatus)
                    {
                        continue;
                    }
                    sflowStatus = true;
                } 
                else if (fvValue(i) == "disable")
                {
                    if(!sflowStatus)
                    {
                        continue;
                    }
                    sflowStatus = false;
                }
                if (!sflowStatusSet(sflowStatus, false))
                {
                    ret = false;
                    continue;
                }
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (!sflowStatusSet(false, true))
        {
            ret = false;
        }
        sflowStatus = false;
    }
    return ret;
}

bool SflowOrch::handleGlobalConfig(KeyOpFieldsValuesTuple tuple)
{
    bool ret = true;
    string op = kfvOp(tuple);

    if (op == SET_COMMAND)
    {
        for (auto i : kfvFieldsValues(tuple))
        {
            if (fvField(i) == "admin_state")
            {
                if(fvValue(i) == "enable")
                {
                    gEnable = true;
                } 
                else if(fvValue(i) == "disable")
                {
                    gEnable = false;
                }
                if(!sflowGlobalConfigure(gEnable)) 
                {
                    ret = false;
                    continue;
                }
            }
            else
            {
                ret = false;
            }
        }
    }
    else if (op == DEL_COMMAND)
    {
        /* By default global configure is true*/
        if (!sflowGlobalConfigure(true))
        {
            ret = false;
        }
        else 
        {
            gEnable = true;
        }
    }
    return ret;
}

void SflowOrch::doSflowStatusTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string op = kfvOp(tuple);

        if (!handleSflowStatus(tuple))
        {
            it++;
            continue;
        } 
        it = consumer.m_toSync.erase(it);
    }
}

void SflowOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    Port port;

    string table_name = consumer.getTableName();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if (table_name == APP_SFLOW_TABLE_NAME)
    {
        doSflowStatusTask(consumer);
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string op = kfvOp(tuple);

        string alias = kfvKey(tuple);

        if (alias == "all") 
        {
            if (handleGlobalConfig(tuple))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
            continue;
        }

        gPortsOrch->getPort(alias, port);

        if (op == SET_COMMAND)
        {
            bool      adminState = gEnable;
            uint32_t  rate       = 0;

            bool      rateSet    = false;
            bool      adminSet   = false;

            sflowGetDefaultSampleRate(port, rate);

            for (auto i : kfvFieldsValues(tuple))
            {
                if (fvField(i) == "admin_state")
                {
                    if (fvValue(i) == "enable")
                    {
                        adminState = true;
                    } 
                    else if(fvValue(i) == "disable")
                    {
                        adminState = false;
                    }
                    adminSet = true;
                }

                if(fvField(i) == "sample_rate")
                {
                    rate = (uint32_t)stoul(fvValue(i));
                    rateSet = true;
                }
            }
            auto sflowInfo = m_sflowPortSessionMap.find(port.m_port_id);

            if (sflowInfo == m_sflowPortSessionMap.end())
            {
                SflowSession session;
                
                session.rate = rate;
                session.adminState = adminState;
                session.globalConfigured = false;
                auto e_sampleObj = m_sflowRateSampleMap.find(rate);

                if (e_sampleObj != m_sflowRateSampleMap.end())
                {
                    session.m_sample_id = e_sampleObj->second;
                }
                else 
                {
                    if (!sflowCreateSession(session))
                    {
                        it++;
                        continue;
                    }
                    m_sflowRateSampleMap[rate] = session.m_sample_id;
                    m_sflowSampleRefMap[session.m_sample_id] = 0;
                }
                if (adminState == true)
                {
                    if (!sflowAddPort(session, port.m_port_id))
                    {
                        it++;
                        continue;
                    }
                }
                m_sflowPortSessionMap[port.m_port_id] = session;
                m_sflowSampleRefMap[session.m_sample_id]++;
            }
            else 
            {
                if ((rateSet) && (rate != sflowInfo->second.rate))
                {
                    if (sflowUpdateRate(port.m_port_id, rate))
                    {
                        it++;
                        continue;
                    }
                }
                if ((adminSet) && (adminState != sflowInfo->second.adminState))
                {
                    if(adminState)
                    {
                        if (!sflowAddPort(sflowInfo->second, port.m_port_id))
                        {
                            it++;
                            continue;
                        }
                    }
                    else
                    {
                        if (!sflowDelPort(sflowInfo->second, port.m_port_id))
                        {
                            it++;
                            continue;
                        }
                    }
                    sflowInfo->second.adminState = adminState;
                }
                sflowInfo->second.globalConfigured = false;
            }
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            auto sflowInfo = m_sflowPortSessionMap.find(port.m_port_id);
            if (sflowInfo != m_sflowPortSessionMap.end())
            {
                if(gEnable)
                {
                    sflowPortApplyGlobalSetting(port, sflowInfo->second);
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                if(sflowInfo->second.adminState)
                {
                    if (!sflowDelPort(sflowInfo->second, port.m_port_id))
                    {
                        it++;
                        continue;
                    }
                    sflowInfo->second.adminState = false;
                }

                m_sflowPortSessionMap.erase(port.m_port_id);
                m_sflowSampleRefMap[sflowInfo->second.m_sample_id]--;
                if (m_sflowSampleRefMap[sflowInfo->second.m_sample_id] == 0)
                {
                    if (!sflowDestroySession(sflowInfo->second))
                    {
                        it++;
                        continue;
                    }
                    m_sflowSampleRefMap.erase(sflowInfo->second.m_sample_id);
                    m_sflowRateSampleMap.erase(sflowInfo->second.rate);
                }
            }
            it = consumer.m_toSync.erase(it);
        }
    }
}
