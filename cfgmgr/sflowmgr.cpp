#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "sflowmgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

map<string,string> sflowSpeedRateInitMap =
{
    {SFLOW_SAMPLE_RATE_KEY_400G, SFLOW_SAMPLE_RATE_VALUE_400G},
    {SFLOW_SAMPLE_RATE_KEY_100G, SFLOW_SAMPLE_RATE_VALUE_100G},
    {SFLOW_SAMPLE_RATE_KEY_50G, SFLOW_SAMPLE_RATE_VALUE_50G},
    {SFLOW_SAMPLE_RATE_KEY_40G, SFLOW_SAMPLE_RATE_VALUE_40G},
    {SFLOW_SAMPLE_RATE_KEY_25G, SFLOW_SAMPLE_RATE_VALUE_25G},
    {SFLOW_SAMPLE_RATE_KEY_10G, SFLOW_SAMPLE_RATE_VALUE_10G},
    {SFLOW_SAMPLE_RATE_KEY_1G, SFLOW_SAMPLE_RATE_VALUE_1G}
};

SflowMgr::SflowMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgSflowTable(cfgDb, CFG_SFLOW_TABLE_NAME),
        m_cfgSflowSessionTable(cfgDb, CFG_SFLOW_SESSION_TABLE_NAME),
        m_appSflowTable(appDb, APP_SFLOW_TABLE_NAME),
        m_appSflowSessionTable(appDb, APP_SFLOW_SESSION_TABLE_NAME)
{
    m_intfAllConf = true;
    m_gEnable = false;
}

void SflowMgr::sflowHandleService(bool enable)
{
    stringstream cmd;
    string res;

    SWSS_LOG_ENTER();

    if (enable)
    {
        cmd << "service hsflowd restart";
    }
    else
    {
        cmd << "service hsflowd stop";
    }

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
    else
    {
        SWSS_LOG_NOTICE("Starting hsflowd service");
        SWSS_LOG_INFO("Command '%s' succeeded", cmd.str().c_str());
    }

}

void SflowMgr::sflowUpdatePortInfo(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        auto values = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            SflowPortInfo port_info;
            bool new_port = false;

            auto sflowPortConf = m_sflowPortConfMap.find(key);
            if (sflowPortConf == m_sflowPortConfMap.end())
            {
                new_port = true;
                port_info.local_conf = false;
                port_info.speed = SFLOW_ERROR_SPEED_STR;
                port_info.rate = "";
                port_info.admin = "";
                m_sflowPortConfMap[key] = port_info;
            }
            for (auto i : values)
            {
                if (fvField(i) == "speed")
                {
                    m_sflowPortConfMap[key].speed = fvValue(i);
                }
            }

            if (new_port)
            {
                if (m_gEnable && m_intfAllConf)
                {
                    vector<FieldValueTuple> fvs;
                    sflowGetGlobalInfo(fvs, m_sflowPortConfMap[key].speed);
                    m_appSflowSessionTable.set(key, fvs);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            auto sflowPortConf = m_sflowPortConfMap.find(key);
            if (sflowPortConf != m_sflowPortConfMap.end())
            {
                bool local_cfg = m_sflowPortConfMap[key].local_conf;

                m_sflowPortConfMap.erase(key);
                if ((m_intfAllConf && m_gEnable) || local_cfg)
                {
                    m_appSflowSessionTable.del(key);
                }
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}

void SflowMgr::sflowHandleSessionAll(bool enable)
{
    for (auto it: m_sflowPortConfMap)
    {
        if (!it.second.local_conf)
        {
            vector<FieldValueTuple> fvs;
            sflowGetGlobalInfo(fvs, it.second.speed);
            if (enable)
            {
                m_appSflowSessionTable.set(it.first, fvs);
            }
            else
            {
                m_appSflowSessionTable.del(it.first);
            }
        }
    }
}

void SflowMgr::sflowHandleSessionLocal(bool enable)
{
    for (auto it: m_sflowPortConfMap)
    {
        if (it.second.local_conf)
        {
            vector<FieldValueTuple> fvs;
            sflowGetPortInfo(fvs, it.second);
            if (enable)
            {
                m_appSflowSessionTable.set(it.first, fvs);
            }
            else
            {
                m_appSflowSessionTable.del(it.first);
            }
        }
    }
}

void SflowMgr::sflowGetGlobalInfo(vector<FieldValueTuple> &fvs, string speed)
{
    string rate;
    FieldValueTuple fv1("admin_state", "up");
    fvs.push_back(fv1);

    if (speed != SFLOW_ERROR_SPEED_STR)
    {
        rate = sflowSpeedRateInitMap[speed];
    }
    else
    {
        rate = SFLOW_ERROR_SPEED_STR;
    }
    FieldValueTuple fv2("sample_rate",rate);
    fvs.push_back(fv2);
}

void SflowMgr::sflowGetPortInfo(vector<FieldValueTuple> &fvs, SflowPortInfo &local_info)
{
    if (local_info.admin.length() > 0)
    {
        FieldValueTuple fv1("admin_state", local_info.admin);
        fvs.push_back(fv1);
    }

    FieldValueTuple fv2("sample_rate", local_info.rate);
    fvs.push_back(fv2);
}

void SflowMgr::sflowCheckAndFillValues(string alias, vector<FieldValueTuple> &fvs)
{
    string rate;
    bool admin_present = false;
    bool rate_present = false;

    for (auto i : fvs)
    {
        if (fvField(i) == "sample_rate")
        {
            rate_present = true;
            m_sflowPortConfMap[alias].rate = fvValue(i);
        }
        if (fvField(i) == "admin_state")
        {
            admin_present = true;
            m_sflowPortConfMap[alias].admin = fvValue(i);
        }
    }

    if (!rate_present)
    {
        if (m_sflowPortConfMap[alias].rate == "")
        {
            string speed = m_sflowPortConfMap[alias].speed;

            if (speed != SFLOW_ERROR_SPEED_STR)
            {
                rate = sflowSpeedRateInitMap[speed];
            }
            else
            {
                rate = SFLOW_ERROR_SPEED_STR;
            }
            m_sflowPortConfMap[alias].rate = rate;
        }
        FieldValueTuple fv("sample_rate", m_sflowPortConfMap[alias].rate);
        fvs.push_back(fv);
    }

    if (!admin_present)
    {
        if (m_sflowPortConfMap[alias].admin == "")
        {
            /* By default admin state is enable if not set explicitely */
            m_sflowPortConfMap[alias].admin = "up";
        }
        FieldValueTuple fv("admin_state", m_sflowPortConfMap[alias].admin);
        fvs.push_back(fv);
    }
}

void SflowMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    if (table == CFG_PORT_TABLE_NAME)
    {
        sflowUpdatePortInfo(consumer);
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        auto values = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            if (table == CFG_SFLOW_TABLE_NAME)
            {
                for (auto i : values)
                {
                    if (fvField(i) == "admin_state")
                    {
                        bool enable = false;
                        if (fvValue(i) == "up")
                        {
                            enable = true;
                        }
                        if (enable == m_gEnable)
                        {
                            break;
                        }
                        m_gEnable = enable;
                        sflowHandleService(enable);
                        if (m_intfAllConf)
                        {
                            sflowHandleSessionAll(enable);
                        }
                        sflowHandleSessionLocal(enable);
                    }
                }
                m_appSflowTable.set(key, values);
            }
            else if (table == CFG_SFLOW_SESSION_TABLE_NAME)
            {
                if (key == "all")
                {
                    for (auto i : values)
                    {
                        if (fvField(i) == "admin_state")
                        {
                            bool enable = false;

                            if (fvValue(i) == "up")
                            {
                                enable = true;
                            }
                            if ((enable != m_intfAllConf) && (m_gEnable))
                            {
                                sflowHandleSessionAll(enable);
                            }
                            m_intfAllConf = enable;
                        }
                    }
                }
                else
                {
                    auto sflowPortConf = m_sflowPortConfMap.find(key);

                    if (sflowPortConf == m_sflowPortConfMap.end())
                    {
                        it++;
                        continue;
                    }
                    sflowCheckAndFillValues(key,values);
                    m_sflowPortConfMap[key].local_conf = true;
                    if (m_gEnable)
                    {
                        m_appSflowSessionTable.set(key, values);
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (table == CFG_SFLOW_TABLE_NAME)
            {
                if (m_gEnable)
                {
                    sflowHandleService(false);
                    sflowHandleSessionAll(false);
                    sflowHandleSessionLocal(false);
                }
                m_gEnable = false;
                m_appSflowTable.del(key);
            }
            else if (table == CFG_SFLOW_SESSION_TABLE_NAME)
            {
                if (key == "all")
                {
                    if (!m_intfAllConf)
                    {
                        if (m_gEnable)
                        {
                            sflowHandleSessionAll(true);
                        }
                    }
                    m_intfAllConf = true;
                }
                else
                {
                    m_appSflowSessionTable.del(key);
                    m_sflowPortConfMap[key].local_conf = false;
                    m_sflowPortConfMap[key].rate = "";
                    m_sflowPortConfMap[key].admin = "";

                    /* If Global configured, set global session on port after local config is deleted */
                    if (m_intfAllConf)
                    {
                        vector<FieldValueTuple> fvs;
                        sflowGetGlobalInfo(fvs, m_sflowPortConfMap[key].speed);
                        m_appSflowSessionTable.set(key,fvs);
                    }
                }
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}
