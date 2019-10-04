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
    intf_all_conf = true;
    gEnable = false;
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
            SflowLocalPortInfo port_info;
            bool new_port = false;

            auto sflowPortConf = m_sflowPortLocalConfMap.find(key);
            if (sflowPortConf == m_sflowPortLocalConfMap.end())
            {
                new_port = true;
                port_info.local_conf = false;
                port_info.speed = SFLOW_ERROR_SPEED_STR;
                port_info.local_rate = "";
                port_info.local_admin = "";
                m_sflowPortLocalConfMap[key] = port_info;
            }
            for (auto i : values)
            {
                if (fvField(i) == "speed")
                {
                    m_sflowPortLocalConfMap[key].speed = fvValue(i);
                }
            }

            if (new_port)
            {
                if (gEnable && intf_all_conf)
                {
                    vector<FieldValueTuple> fvs;
                    sflowGetGlobalFvs(fvs, m_sflowPortLocalConfMap[key].speed);
                    m_appSflowSessionTable.set(key, fvs);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            auto sflowPortConf = m_sflowPortLocalConfMap.find(key);
            if (sflowPortConf != m_sflowPortLocalConfMap.end())
            {
                bool local_cfg = m_sflowPortLocalConfMap[key].local_conf;

                m_sflowPortLocalConfMap.erase(key);
                if ((intf_all_conf && gEnable) || local_cfg)
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
    for (auto it: m_sflowPortLocalConfMap)
    {
        if (!it.second.local_conf)
        {
            vector<FieldValueTuple> fvs;
            sflowGetGlobalFvs(fvs, it.second.speed);
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
    for (auto it: m_sflowPortLocalConfMap)
    {
        if (it.second.local_conf)
        {
            vector<FieldValueTuple> fvs;
            sflowGetLocalFvs(fvs, it.second);
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

void SflowMgr::sflowGetGlobalFvs(vector<FieldValueTuple> &fvs, string speed)
{
    string rate;
    FieldValueTuple fv1("admin_state", "enable");
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

void SflowMgr::sflowGetLocalFvs(vector<FieldValueTuple> &fvs, SflowLocalPortInfo &local_info)
{
    if (local_info.local_admin.length() > 0)
    {
        FieldValueTuple fv1("admin_state", local_info.local_admin);
        fvs.push_back(fv1);
    }

    FieldValueTuple fv2("sample_rate", local_info.local_rate);
    fvs.push_back(fv2);
}

void SflowMgr::sflowUpdateLocalPortInfo(string alias, vector<FieldValueTuple> &fvs)
{
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_state")
        {
            m_sflowPortLocalConfMap[alias].local_admin = fvValue(i);
        }
        else if (fvField(i) == "sample_rate")
        {
            m_sflowPortLocalConfMap[alias].local_rate = fvValue(i);
        }
    }
}

void SflowMgr::sflowCheckAndFillRate(string alias, vector<FieldValueTuple> &fvs)
{
    string rate;

    for (auto i : fvs)
    {
        if (fvField(i) == "sample_rate")
        {
            /* Rate exists already. */
            return;
        }
    }
    string speed = m_sflowPortLocalConfMap[alias].speed;

    if (speed != SFLOW_ERROR_SPEED_STR)
    {
        rate = sflowSpeedRateInitMap[speed];
    }
    else
    {
        rate = SFLOW_ERROR_SPEED_STR;
    }
    m_sflowPortLocalConfMap[alias].local_rate = rate;
    FieldValueTuple fv("sample_rate",rate);
    fvs.push_back(fv);
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
                        if (fvValue(i) == "enable")
                        {
                            enable = true;
                        }
                        if (enable == gEnable)
                        {
                            break;
                        }
                        gEnable = enable;
                        sflowHandleService(enable);
                        if (intf_all_conf)
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

                            if (fvValue(i) == "enable")
                            {
                                enable = true;
                            }
                            if ((enable != intf_all_conf) && (gEnable))
                            {
                                sflowHandleSessionAll(enable);
                            }
                            intf_all_conf = enable;
                        }
                    }
                }
                else
                {
                    auto sflowPortConf = m_sflowPortLocalConfMap.find(key);

                    if (sflowPortConf == m_sflowPortLocalConfMap.end())
                    {
                        it++;
                        continue;
                    }
                    if ((m_sflowPortLocalConfMap[key].local_rate == "") ||
                        (m_sflowPortLocalConfMap[key].local_rate == SFLOW_ERROR_SPEED_STR))
                    {
                        sflowCheckAndFillRate(key,values);
                    }
                    sflowUpdateLocalPortInfo(key,values);
                    m_sflowPortLocalConfMap[key].local_conf = true;
                    m_appSflowSessionTable.set(key, values);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (table == CFG_SFLOW_TABLE_NAME)
            {
                if (gEnable)
                {
                    sflowHandleService(false);
                    sflowHandleSessionAll(false);
                }
                gEnable = false;
                m_appSflowTable.del(key);
            }
            else if (table == CFG_SFLOW_SESSION_TABLE_NAME)
            {
                if (key == "all")
                {
                    if (!intf_all_conf)
                    {
                        sflowHandleSessionAll(true);
                    }
                    intf_all_conf = true;
                }
                else
                {
                    m_appSflowSessionTable.del(key);
                    m_sflowPortLocalConfMap[key].local_conf = false;
                    m_sflowPortLocalConfMap[key].local_rate = "";
                    m_sflowPortLocalConfMap[key].local_admin = "";

                    /* If Global configured, set global session on port after local config is deleted */
                    if (intf_all_conf)
                    {
                        vector<FieldValueTuple> fvs;
                        sflowGetGlobalFvs(fvs, m_sflowPortLocalConfMap[key].speed);
                        m_appSflowSessionTable.set(key,fvs);
                    }
                }
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}
