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
    {SFLOW_SAMPLE_RATE_KEY_200G, SFLOW_SAMPLE_RATE_VALUE_200G},
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
    m_gDirection = "rx";
    m_intfAllDir = "rx";
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
                port_info.local_rate_cfg = false;
                port_info.local_admin_cfg = false;
                port_info.local_dir_cfg = false;
                port_info.speed = SFLOW_ERROR_SPEED_STR;
                port_info.rate = "";
                port_info.admin = "";
                port_info.dir = "";
                m_sflowPortConfMap[key] = port_info;
            }

            bool speed_change = false;
            string new_speed = SFLOW_ERROR_SPEED_STR;
            for (auto i : values)
            {
                if (fvField(i) == "speed")
                {
                    new_speed = fvValue(i);
                }
            }
            if (m_sflowPortConfMap[key].speed != new_speed)
            {
                m_sflowPortConfMap[key].speed = new_speed;
                speed_change = true;
            }

            string def_dir = "rx";
            if (m_sflowPortConfMap[key].dir != def_dir && !m_sflowPortConfMap[key].local_dir_cfg)
            {
                m_sflowPortConfMap[key].dir = def_dir;
            }

            if (m_gEnable && m_intfAllConf)
            {
                // If the Local rate Conf is already present, dont't override it even though the speed is changed
                if (new_port || (speed_change && !m_sflowPortConfMap[key].local_rate_cfg))
                {
                    vector<FieldValueTuple> fvs;
                    sflowGetGlobalInfo(fvs, m_sflowPortConfMap[key].speed, m_sflowPortConfMap[key].dir);
                    m_appSflowSessionTable.set(key, fvs);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            auto sflowPortConf = m_sflowPortConfMap.find(key);
            if (sflowPortConf != m_sflowPortConfMap.end())
            {
                bool local_cfg = m_sflowPortConfMap[key].local_rate_cfg ||
                                 m_sflowPortConfMap[key].local_admin_cfg ||
                                 m_sflowPortConfMap[key].local_dir_cfg;

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

void SflowMgr::sflowHandleSessionAll(bool enable, string direction)
{
    for (auto it: m_sflowPortConfMap)
    {
        if (enable)
        {
            vector<FieldValueTuple> fvs;
            if (it.second.local_rate_cfg || it.second.local_admin_cfg || it.second.local_dir_cfg)
            {
                sflowGetPortInfo(fvs, it.second);
                /* Use global admin state if there is not a local one */
                if (!it.second.local_admin_cfg) {
                    FieldValueTuple fv1("admin_state", "up");
                    fvs.push_back(fv1);
                }

                /* Use global sample direction state if there is not a local one */
                if (!it.second.local_dir_cfg) {
                    FieldValueTuple fv2("sample_direction", direction);
                    fvs.push_back(fv2);
                }
            }
            else
            {
                sflowGetGlobalInfo(fvs, it.second.speed, direction);
            }
            m_appSflowSessionTable.set(it.first, fvs);
        }
        else if (!it.second.local_admin_cfg)
        {
            m_appSflowSessionTable.del(it.first);
        }
    }
}

void SflowMgr::sflowHandleSessionLocal(bool enable)
{
    for (auto it: m_sflowPortConfMap)
    {
        if (it.second.local_admin_cfg || it.second.local_rate_cfg || it.second.local_dir_cfg)
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

void SflowMgr::sflowGetGlobalInfo(vector<FieldValueTuple> &fvs, string speed, string dir)
{
    string rate;
    FieldValueTuple fv1("admin_state", "up");
    fvs.push_back(fv1);

    if (speed != SFLOW_ERROR_SPEED_STR && sflowSpeedRateInitMap.find(speed) != sflowSpeedRateInitMap.end())
    {
        rate = sflowSpeedRateInitMap[speed];
    }
    else
    {
        rate = SFLOW_ERROR_SPEED_STR;
    }
    FieldValueTuple fv2("sample_rate",rate);
    fvs.push_back(fv2);

    FieldValueTuple fv3("sample_direction",dir);
    fvs.push_back(fv3);
}

void SflowMgr::sflowGetPortInfo(vector<FieldValueTuple> &fvs, SflowPortInfo &local_info)
{
    if (local_info.local_admin_cfg)
    {
        FieldValueTuple fv1("admin_state", local_info.admin);
        fvs.push_back(fv1);
    }

    FieldValueTuple fv2("sample_rate", local_info.rate);
    fvs.push_back(fv2);

    if (local_info.local_dir_cfg)
    {
        FieldValueTuple fv3("sample_direction", local_info.dir);
        fvs.push_back(fv3);
    }
}

void SflowMgr::sflowCheckAndFillValues(string alias, vector<FieldValueTuple> &values,
                                       vector<FieldValueTuple> &fvs)
{
    string rate;
    bool admin_present = false;
    bool rate_present = false;
    bool dir_present = false;

    for (auto i : values)
    {
        if (fvField(i) == "sample_rate")
        {
            rate_present = true;
            m_sflowPortConfMap[alias].rate = fvValue(i);
            m_sflowPortConfMap[alias].local_rate_cfg = true;
            FieldValueTuple fv(fvField(i), fvValue(i));
            fvs.push_back(fv);
        }
        if (fvField(i) == "admin_state")
        {
            admin_present = true;
            m_sflowPortConfMap[alias].admin = fvValue(i);
            m_sflowPortConfMap[alias].local_admin_cfg = true;
            FieldValueTuple fv(fvField(i), fvValue(i));
            fvs.push_back(fv);
        }
        if (fvField(i) == "sample_direction")
        {
            dir_present = true;
            m_sflowPortConfMap[alias].dir = fvValue(i);
            m_sflowPortConfMap[alias].local_dir_cfg = true;
            FieldValueTuple fv(fvField(i), fvValue(i));
            fvs.push_back(fv);
        }
        if (fvField(i) == "NULL")
        {
            continue;
        }
    }

    if (!rate_present)
    {
        /* Go back to default sample-rate if there is not existing rate OR
         * if a local config has been done but the rate has been removed
         */
        if (m_sflowPortConfMap[alias].rate == "" ||
            m_sflowPortConfMap[alias].local_rate_cfg)
        {
            string speed = m_sflowPortConfMap[alias].speed;

            if (speed != SFLOW_ERROR_SPEED_STR && sflowSpeedRateInitMap.find(speed) != sflowSpeedRateInitMap.end())
            {
                rate = sflowSpeedRateInitMap[speed];
            }
            else
            {
                rate = SFLOW_ERROR_SPEED_STR;
            }
            m_sflowPortConfMap[alias].rate = rate;
        }
        m_sflowPortConfMap[alias].local_rate_cfg = false;
        FieldValueTuple fv("sample_rate", m_sflowPortConfMap[alias].rate);
        fvs.push_back(fv);
    }

    if (!admin_present)
    {
        if (m_sflowPortConfMap[alias].admin == "")
        {
            /* By default admin state is enabled if not set explicitly */
            m_sflowPortConfMap[alias].admin = "up";
        }
        m_sflowPortConfMap[alias].local_admin_cfg = false;
        FieldValueTuple fv("admin_state", m_sflowPortConfMap[alias].admin);
        fvs.push_back(fv);
    }

    if (!dir_present)
    {
        if (m_sflowPortConfMap[alias].dir == "")
        {
            /* By default direction is set to global, if not set explicitly */
            m_sflowPortConfMap[alias].dir = m_gDirection;
        }
        m_sflowPortConfMap[alias].local_dir_cfg = false;
        FieldValueTuple fv("sample_direction", m_sflowPortConfMap[alias].dir);
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
                SWSS_LOG_DEBUG("Current Cfg admin %d dir %s ", (unsigned int)m_gEnable, m_gDirection.c_str());
                bool enable = false;
                string direction = "rx";
                for (auto i : values)
                {
                    if (fvField(i) == "admin_state")
                    {
                        if (fvValue(i) == "up")
                        {
                            enable = true;
                        }
                    }
                    else if (fvField(i) == "sample_direction")
                    {
                        direction = fvValue(i);
                    }
                }

                if (direction != m_gDirection)
                {
                    m_gDirection = direction;
                }

                if (m_gEnable != enable)
                {
                    m_gEnable = enable;
                    sflowHandleService(enable);
                }

                if (m_intfAllConf)
                {
                    sflowHandleSessionAll(m_gEnable, m_gDirection);
                }

                sflowHandleSessionLocal(m_gEnable);
                m_appSflowTable.set(key, values);

                SWSS_LOG_DEBUG("New config admin %d dir %s ", (unsigned int)m_gEnable, m_gDirection.c_str());
            }
            else if (table == CFG_SFLOW_SESSION_TABLE_NAME)
            {
                if (key == "all")
                {
                    SWSS_LOG_DEBUG("current config gAdmin %d dir %s intfAllEna %d intfAllDir %s",
                        (unsigned int)m_gEnable, m_gDirection.c_str(),
                        (unsigned int)m_intfAllConf, m_intfAllDir.c_str());

                    string direction = m_intfAllDir;
                    bool enable = m_intfAllConf;
                    for (auto i : values)
                    {
                        if (fvField(i) == "admin_state")
                        {
                            if (fvValue(i) == "up")
                            {
                                enable = true;
                            }
                            else if (fvValue(i) == "down")
                            {
                                enable = false;
                            }
                        }
                        else if (fvField(i) == "sample_direction")
                        {
                            direction = fvValue(i);
                        }
                    }

                    if (m_intfAllDir != direction)
                    {
                        m_intfAllDir = direction;
                    }

                    if (enable != m_intfAllConf)
                    {
                        m_intfAllConf = enable;
                    }

                    if (m_gEnable)
                    {
                        sflowHandleSessionAll(m_intfAllConf, m_intfAllDir);
                    }

                    SWSS_LOG_DEBUG("New config gAdmin %d dir %s intfAllEna %d intfAllDir %s",
                        (unsigned int)m_gEnable, m_gDirection.c_str(),
                        (unsigned int)m_intfAllConf, m_intfAllDir.c_str());
                }
                else
                {
                    auto sflowPortConf = m_sflowPortConfMap.find(key);

                    if (sflowPortConf == m_sflowPortConfMap.end())
                    {
                        it++;
                        continue;
                    }
                    vector<FieldValueTuple> fvs;
                    sflowCheckAndFillValues(key, values, fvs);
                    if (m_gEnable)
                    {
                        m_appSflowSessionTable.set(key, fvs);
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
                    sflowHandleSessionAll(false, "");
                    sflowHandleSessionLocal(false);
                }
                m_gEnable = false;
                m_gDirection = "rx";
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
                            sflowHandleSessionAll(true, m_gDirection);
                        }
                    }
                    m_intfAllConf = true;
                }
                else
                {
                    m_appSflowSessionTable.del(key);
                    m_sflowPortConfMap[key].local_rate_cfg = false;
                    m_sflowPortConfMap[key].local_admin_cfg = false;
                    m_sflowPortConfMap[key].local_dir_cfg = false;
                    m_sflowPortConfMap[key].rate = "";
                    m_sflowPortConfMap[key].admin = "";
                    m_sflowPortConfMap[key].dir = "";

                    /* If Global configured, set global session on port after local config is deleted */
                    if (m_intfAllConf)
                    {
                        vector<FieldValueTuple> fvs;
                        sflowGetGlobalInfo(fvs, m_sflowPortConfMap[key].speed, m_intfAllDir);
                        m_appSflowSessionTable.set(key,fvs);
                    }
                }
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}
