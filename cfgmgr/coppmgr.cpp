#include <fstream>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "coppmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include "warm_restart.h"
#include "json.hpp"

using json = nlohmann::json;

using namespace std;
using namespace swss;

static set<string> g_copp_init_set;
static map<string, string> trap_feature_map = {
    {COPP_TRAP_TYPE_SAMPLEPACKET, "sflow"}
};

void CoppMgr::parseInitFile(void)
{
    std::ifstream ifs(COPP_INIT_FILE);
    if (ifs.fail())
    {
        return; 
    }
    json j = json::parse(ifs);
    for(auto tbl = j.begin(); tbl != j.end(); tbl++)
    {
        string table_name = tbl.key();
        json keys = tbl.value();
        for (auto k = keys.begin(); k != keys.end(); k++)
        {
            string table_key = k.key();
            json fvp = k.value();
            vector<FieldValueTuple> fvs;

            for (auto f = fvp.begin(); f != fvp.end(); f++)
            {
                FieldValueTuple fv(f.key(), f.value().get<std::string>());
                fvs.push_back(fv);
            }
            if (table_name == CFG_COPP_TRAP_TABLE_NAME)
            {
                m_coppTrapInitCfg[table_key] = fvs;
            }
            else if (table_name == CFG_COPP_GROUP_TABLE_NAME)
            {
                m_coppGroupInitCfg[table_key] = fvs;
            }
        }
    }
}

/* Check if the trap group has traps that can be installed only when
 * feature is enabled
 */
bool CoppMgr::checkIfTrapGroupFeaturePending(string trap_group_name)
{
    std::vector<FieldValueTuple> fvs;

    if (m_coppGroupRestrictedMap.find(trap_group_name) == m_coppGroupRestrictedMap.end())
    {
        return false;
    } 

    for (auto it: m_coppTrapIdTrapGroupMap)
    {
        if (it.second == trap_group_name)
        {
            /* At least one trap should be enabled to install the 
             * trap group with restricted attributes
             */ 
            if (!isTrapDisabled(it.first))
            {
                return false;
            }
        }
    }
    return true;
}

void CoppMgr::setFeatureTrapIdsStatus(string feature, bool enable)
{
    for (auto it: trap_feature_map)
    {
        if (it.second ==  feature)
        {
            if (enable)
            {
                if (m_coppTrapDisabledMap.find(it.first) != m_coppTrapDisabledMap.end())
                {
                    if (m_coppTrapIdTrapGroupMap.find(it.first) != m_coppTrapIdTrapGroupMap.end())
                    {
                        string trap_group = m_coppTrapIdTrapGroupMap[it.first];

                        /* Enable only when the restricted group is already pending */
                        if ((m_coppGroupRestrictedMap.find(trap_group) 
                             != m_coppGroupRestrictedMap.end()) &&
                             (checkIfTrapGroupFeaturePending(trap_group)))
                        {
                            vector<FieldValueTuple> fvs;
                            vector<FieldValueTuple> modified_fvs;
                            string trap_ids;

                            for (auto i : m_coppGroupRestrictedMap[trap_group])
                            {
                                FieldValueTuple fv(i.first, i.second);
                                fvs.push_back(fv);
                            }
                            getTrapGroupTrapIds(trap_group, trap_ids);
                            if (!trap_ids.empty())
                            {
                                FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                                fvs.push_back(fv);
                            }
                            coppGroupGetModifiedFvs (trap_group, fvs, modified_fvs, false);
                            if (!modified_fvs.empty())
                            {
                                m_appCoppTable.set(trap_group, modified_fvs);
                            }

                        }
                    }
                    m_coppTrapDisabledMap.erase(it.first);
                }
            }
            else
            {
                if (m_coppTrapDisabledMap.find(it.first) == m_coppTrapDisabledMap.end())
                {
                    m_coppTrapDisabledMap[it.first] = true;
                    if (m_coppTrapIdTrapGroupMap.find(it.first) != m_coppTrapIdTrapGroupMap.end())
                    {
                        string trap_group = m_coppTrapIdTrapGroupMap[it.first];

                        /* Delete the restricted copp group when it goes to pending state
                         * on feature disable
                         */
                        if ((m_coppGroupRestrictedMap.find(trap_group) 
                             != m_coppGroupRestrictedMap.end()) &&
                             (checkIfTrapGroupFeaturePending(trap_group)))
                        {
                            m_appCoppTable.del(trap_group);
                        }
                    }
                }
            }
        }
    }
}

bool CoppMgr::isTrapDisabled(string trap_id)
{
    auto trap_idx = m_coppTrapDisabledMap.find(trap_id);
    if (trap_idx != m_coppTrapDisabledMap.end())
    {
        return m_coppTrapDisabledMap[trap_id];
    }
    return false;
}

/* The genetlink  fields are restricted and needs to be installed only on
 * feature(sflow) enable
 */
bool CoppMgr::coppGroupHasRestrictedFields (vector<FieldValueTuple> &fvs)
{
    for (auto i: fvs)
    {
        if ((fvField(i) == COPP_GROUP_GENETLINK_NAME_FIELD) ||
            (fvField(i) == COPP_GROUP_GENETLINK_MCGRP_NAME_FIELD))
        {
            return true;
        }
    }
    return false;
}

CoppMgr::CoppMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgCoppTrapTable(cfgDb, CFG_COPP_TRAP_TABLE_NAME),
        m_cfgCoppGroupTable(cfgDb, CFG_COPP_GROUP_TABLE_NAME),
        m_cfgFeatureTable(cfgDb, CFG_FEATURE_TABLE_NAME),
        m_appCoppTable(appDb, APP_COPP_TABLE_NAME),
        m_stateCoppTrapTable(stateDb, STATE_COPP_TRAP_TABLE_NAME),
        m_stateCoppGroupTable(stateDb, STATE_COPP_GROUP_TABLE_NAME),
        m_coppTable(appDb, APP_COPP_TABLE_NAME)
{
    SWSS_LOG_ENTER();
    parseInitFile();
    std::vector<string> group_keys;
    std::vector<string> trap_keys;
    std::vector<string> feature_keys;
    std::vector<string> app_keys;

    std::vector<string> group_cfg_keys;
    std::vector<string> trap_cfg_keys;

    CoppCfg group_cfg;
    CoppCfg trap_cfg;

    m_cfgCoppGroupTable.getKeys(group_cfg_keys);
    m_cfgCoppTrapTable.getKeys(trap_cfg_keys);
    m_cfgFeatureTable.getKeys(feature_keys);
    m_coppTable.getKeys(app_keys);


    for (auto it: trap_feature_map)
    {
        m_coppTrapDisabledMap[it.first] = true;
    }
    for (auto i: feature_keys)
    {
        std::vector<FieldValueTuple> feature_fvs;
        m_cfgFeatureTable.get(i, feature_fvs);

        bool status = false;
        for (auto j: feature_fvs)
        {
            if (fvField(j) == "status")
            {
                if (fvValue(j) == "enabled")
                {
                    status = true;
                }
            }
        }
        setFeatureTrapIdsStatus(i, status);
    }

    /* Read the init configuration first. If the same key is present in
     * user configuration, override the init fields with user fields
     */
    for (auto i : m_coppTrapInitCfg)
    {
        std::vector<FieldValueTuple> trap_init_fvs = i.second;
        std::vector<FieldValueTuple> trap_fvs;
        auto key = std::find(trap_cfg_keys.begin(), trap_cfg_keys.end(), i.first);
        bool null_cfg = false;
        if (key != trap_cfg_keys.end())
        {
            std::vector<FieldValueTuple> trap_cfg_fvs;
            m_cfgCoppTrapTable.get(i.first, trap_cfg_fvs);

            trap_fvs = trap_cfg_fvs;
            for (auto it1: trap_init_fvs)
            {
                bool field_found = false;
                for (auto it2: trap_cfg_fvs)
                {
                    if(fvField(it2) == "NULL")
                    {
                        SWSS_LOG_DEBUG("Ignoring trap create for key %s",i.first.c_str());
                        null_cfg = true;
                        break;
                    }
                    if(fvField(it1) == fvField(it2))
                    {
                        field_found = true;
                        break;
                    }
                }
                if (!field_found)
                {
                    trap_fvs.push_back(it1);
                }
            }
            if (!null_cfg)
            {
                trap_cfg[i.first] = trap_fvs;
            }
        }
        else
        {
            trap_cfg[i.first] = m_coppTrapInitCfg[i.first];
        }
    }

    /* Read the user configuration keys that were not present in 
     * init configuration.
     */
    for (auto i : trap_cfg_keys)
    {
        if(m_coppTrapInitCfg.find(i) == m_coppTrapInitCfg.end())
        {
            std::vector<FieldValueTuple> trap_cfg_fvs;
            m_cfgCoppTrapTable.get(i, trap_cfg_fvs);
            trap_cfg[i] = trap_cfg_fvs;
        }
    }

    for (auto i : trap_cfg)
    {
        string trap_group;
        string trap_ids;
        std::vector<FieldValueTuple> trap_fvs = i.second;

        for (auto j: trap_fvs)
        {
            if (fvField(j) == COPP_TRAP_ID_LIST_FIELD)
            {
                trap_ids = fvValue(j);
            }
            else if (fvField(j) == COPP_TRAP_GROUP_FIELD)
            {
                trap_group = fvValue(j);
            }
        }
        if (!trap_group.empty() && !trap_ids.empty())
        {
            addTrapIdsToTrapGroup(trap_group, trap_ids);
            m_coppTrapConfMap[i.first].trap_group = trap_group;
            m_coppTrapConfMap[i.first].trap_ids = trap_ids;
        }
    }

    /* Read the init configuration first. If the same key is present in
     * user configuration, override the init fields with user fields
     */
    for (auto i : m_coppGroupInitCfg)
    {
        std::vector<FieldValueTuple> group_init_fvs = i.second;
        std::vector<FieldValueTuple> group_fvs;
        auto key = std::find(group_cfg_keys.begin(), group_cfg_keys.end(), i.first);
        if (key != group_cfg_keys.end())
        {
            std::vector<FieldValueTuple> group_cfg_fvs;
            m_cfgCoppGroupTable.get(i.first, group_cfg_fvs);

            group_fvs = group_cfg_fvs;
            for (auto it1: group_init_fvs)
            {
                bool field_found = false;
                for (auto it2: group_cfg_fvs)
                {
                    if(fvField(it1) == fvField(it2))
                    {
                        field_found = true;
                        break;
                    }
                }
                if (!field_found)
                {
                    group_fvs.push_back(it1);
                }
            }
            group_cfg[i.first] = group_fvs;
        }
        else
        {
            group_cfg[i.first] = m_coppGroupInitCfg[i.first];
        }
    }

    /* Read the user configuration keys that were not present in 
     * init configuration.
     */
    for (auto i : group_cfg_keys)
    {
        if(m_coppGroupInitCfg.find(i) == m_coppGroupInitCfg.end())
        {
            std::vector<FieldValueTuple> group_cfg_fvs;
            m_cfgCoppGroupTable.get(i, group_cfg_fvs);
            group_cfg[i] = group_cfg_fvs;
        }
    }
    for (auto i: group_cfg)
    {
        string trap_ids;
        vector<FieldValueTuple> trap_group_fvs = i.second;

        if (coppGroupHasRestrictedFields (trap_group_fvs))
        {
            for (auto fv: trap_group_fvs)
            {
                m_coppGroupRestrictedMap[i.first][fvField(fv)]= fvValue(fv);
            }
        }
        if (checkIfTrapGroupFeaturePending(i.first))
        {
            continue;
        }

        getTrapGroupTrapIds(i.first, trap_ids);
        if (!trap_ids.empty())
        {
            FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
            trap_group_fvs.push_back(fv);
        }

        vector<FieldValueTuple> trap_app_fvs;

        coppGroupGetModifiedFvs (i.first, trap_group_fvs, trap_app_fvs, true);
        if (!trap_app_fvs.empty())
        {
            m_appCoppTable.set(i.first, trap_app_fvs);
        }
        auto g_cfg = std::find(group_cfg_keys.begin(), group_cfg_keys.end(), i.first);
        if (g_cfg != group_cfg_keys.end())
        {
            g_copp_init_set.insert(i.first);
        }
    }
}

void CoppMgr::setCoppGroupStateOk(string alias)
{
    FieldValueTuple tuple("state", "ok");
    vector<FieldValueTuple> fvs;
    fvs.push_back(tuple);
    m_stateCoppGroupTable.set(alias, fvs);
    SWSS_LOG_NOTICE("Publish %s(ok) to state db", alias.c_str());
}

void CoppMgr::delCoppGroupStateOk(string alias)
{
    m_stateCoppGroupTable.del(alias);
    SWSS_LOG_NOTICE("Delete %s(ok) from state db", alias.c_str());
}

void CoppMgr::setCoppTrapStateOk(string alias)
{
    FieldValueTuple tuple("state", "ok");
    vector<FieldValueTuple> fvs;
    fvs.push_back(tuple);
    m_stateCoppTrapTable.set(alias, fvs);
    SWSS_LOG_NOTICE("Publish %s(ok) to state db", alias.c_str());
}

void CoppMgr::delCoppTrapStateOk(string alias)
{
    m_stateCoppTrapTable.del(alias);
    SWSS_LOG_NOTICE("Delete %s(ok) from state db", alias.c_str());
}

void CoppMgr::addTrapIdsToTrapGroup(string trap_group, string trap_ids)
{
    vector<string> trap_id_list;

    trap_id_list = tokenize(trap_ids, list_item_delimiter);

    for (auto i: trap_id_list)
    {
        m_coppTrapIdTrapGroupMap[i] = trap_group;
    }
}

void CoppMgr::removeTrapIdsFromTrapGroup(string trap_group, string trap_ids)
{
    vector<string> trap_id_list;

    trap_id_list = tokenize(trap_ids, list_item_delimiter);

    for (auto i: trap_id_list)
    {
        m_coppTrapIdTrapGroupMap.erase(i);
    }
}

void CoppMgr::getTrapGroupTrapIds(string trap_group, string &trap_ids)
{
 
    trap_ids.clear();   
    for (auto it: m_coppTrapIdTrapGroupMap)
    {
        if (it.second == trap_group)
        {
            if (trap_ids.empty())
            {
                trap_ids = it.first;
            }
            else
            {
                trap_ids += list_item_delimiter + it.first;
            }
        }
    }
}

void CoppMgr::doCoppTrapTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fvs;
        string trap_ids;
        string trap_group;
        bool   conf_present = false;

        if (op == SET_COMMAND)
        {
            /*Create case*/
            if (m_coppTrapConfMap.find(key) != m_coppTrapConfMap.end())
            {
                trap_ids = m_coppTrapConfMap[key].trap_ids;
                trap_group = m_coppTrapConfMap[key].trap_group;
                conf_present = true;
            }

            bool null_cfg = false;
            for (auto i: kfvFieldsValues(t))
            {
                if (fvField(i) == COPP_TRAP_GROUP_FIELD)
                {
                    trap_group = fvValue(i);
                }
                else if (fvField(i) == COPP_TRAP_ID_LIST_FIELD)
                {
                    trap_ids = fvValue(i);
                }
                else if (fvField(i) == "NULL")
                {
                    null_cfg = true;
                }
            }
            if (null_cfg)
            {
                if (!m_coppTrapConfMap[key].trap_group.empty())
                {
                    SWSS_LOG_DEBUG("Deleting trap key %s", key.c_str());
                    removeTrapIdsFromTrapGroup(m_coppTrapConfMap[key].trap_group,
                            m_coppTrapConfMap[key].trap_ids);
                    trap_ids.clear();
                    getTrapGroupTrapIds(m_coppTrapConfMap[key].trap_group, trap_ids);
                    fvs.clear();
                    FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                    fvs.push_back(fv);
                    if (!checkIfTrapGroupFeaturePending(m_coppTrapConfMap[key].trap_group))
                    {
                        m_appCoppTable.set(m_coppTrapConfMap[key].trap_group, fvs);
                    }
                    m_coppTrapConfMap[key].trap_group = "";
                    m_coppTrapConfMap[key].trap_ids = "";
                }
                it = consumer.m_toSync.erase(it);
                continue;
            }
            /*Duplicate check*/
            if (conf_present &&
                (trap_group == m_coppTrapConfMap[key].trap_group) &&
                (trap_ids == m_coppTrapConfMap[key].trap_ids))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            removeTrapIdsFromTrapGroup(m_coppTrapConfMap[key].trap_group,
                                       m_coppTrapConfMap[key].trap_ids);
            fvs.clear();
            string trap_group_trap_ids;
            addTrapIdsToTrapGroup(trap_group, trap_ids);
            getTrapGroupTrapIds(trap_group, trap_group_trap_ids);
            FieldValueTuple fv1(COPP_TRAP_ID_LIST_FIELD, trap_group_trap_ids);
            fvs.push_back(fv1);
            if (!checkIfTrapGroupFeaturePending(trap_group))
            {
                m_appCoppTable.set(trap_group, fvs);
            }

            /* When the trap table's trap group is changed, the old trap group
             * should also be reprogrammed as some of its associated traps got
             * removed
             */
            if ((!m_coppTrapConfMap[key].trap_group.empty()) && 
                (trap_group != m_coppTrapConfMap[key].trap_group))
            {
                trap_group_trap_ids.clear();
                fvs.clear();
                getTrapGroupTrapIds(m_coppTrapConfMap[key].trap_group, trap_group_trap_ids);
                FieldValueTuple fv2(COPP_TRAP_ID_LIST_FIELD, trap_group_trap_ids);
                fvs.push_back(fv2);
                if (!checkIfTrapGroupFeaturePending(m_coppTrapConfMap[key].trap_group))
                {
                    m_appCoppTable.set(m_coppTrapConfMap[key].trap_group, fvs);
                }
            }
            m_coppTrapConfMap[key].trap_group = trap_group;
            m_coppTrapConfMap[key].trap_ids = trap_ids;
            setCoppTrapStateOk(key);
        }
        else if (op == DEL_COMMAND)
        {
            removeTrapIdsFromTrapGroup(m_coppTrapConfMap[key].trap_group,
                                       m_coppTrapConfMap[key].trap_ids);
            fvs.clear();
            trap_ids.clear();
            if (!m_coppTrapConfMap[key].trap_group.empty())
            {
                getTrapGroupTrapIds(m_coppTrapConfMap[key].trap_group, trap_ids);
                FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                fvs.push_back(fv);
                if (!checkIfTrapGroupFeaturePending(m_coppTrapConfMap[key].trap_group))
                {
                    m_appCoppTable.set(m_coppTrapConfMap[key].trap_group, fvs);
                }
            }
            m_coppTrapConfMap.erase(key);
            delCoppTrapStateOk(key);

            /* If the COPP trap was part of init config, it needs to be recreated
             * with field values from init. The configuration delete should just clear
             * the externally applied user configuration
             */
            if (m_coppTrapInitCfg.find(key) != m_coppTrapInitCfg.end())
            {
                auto fvs = m_coppTrapInitCfg[key];
                for (auto i: fvs)
                {
                    if (fvField(i) == COPP_TRAP_GROUP_FIELD)
                    {
                        trap_group = fvValue(i);
                    }
                    else if (fvField(i) == COPP_TRAP_ID_LIST_FIELD)
                    {
                        trap_ids = fvValue(i);
                    }
                }
                vector<FieldValueTuple> g_fvs;
                string trap_group_trap_ids;
                addTrapIdsToTrapGroup(trap_group, trap_ids);
                getTrapGroupTrapIds(trap_group, trap_group_trap_ids);
                FieldValueTuple fv1(COPP_TRAP_ID_LIST_FIELD, trap_group_trap_ids);
                g_fvs.push_back(fv1);
                if (!checkIfTrapGroupFeaturePending(trap_group))
                {
                    m_appCoppTable.set(trap_group, g_fvs);
                }
                m_coppTrapConfMap[key].trap_group = trap_group;
                m_coppTrapConfMap[key].trap_ids = trap_ids;
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}

/* This API is used to fetch only the modified configurations. During warmboot
 * when certain fields in APP table are not present in new init config file
 * the APP table needs to be removed and recreated
 */
void CoppMgr::coppGroupGetModifiedFvs(string key, vector<FieldValueTuple> &trap_group_fvs, 
                                      vector<FieldValueTuple> &modified_fvs, bool del_on_field_remove)
{
    vector<FieldValueTuple> app_fvs;
    std::vector<string> app_keys;

    m_coppTable.get(key,app_fvs);
    modified_fvs = trap_group_fvs;

    m_coppTable.getKeys(app_keys);
    auto app_key = std::find(app_keys.begin(), app_keys.end(),key);
    if (app_key != app_keys.end())
    {
        vector<FieldValueTuple> app_fvs;
        m_coppTable.get(key, app_fvs);
        for (auto app_idx: app_fvs)
        {
            bool field_removed = true;
            for (auto cfg_idx: trap_group_fvs)
            {
                if (fvField(cfg_idx) == fvField(app_idx))
                {
                    field_removed = false;
                    if (fvValue(cfg_idx) == fvValue(app_idx))
                    {
                        auto vec_idx = std::find(modified_fvs.begin(), modified_fvs.end(), cfg_idx);
                        modified_fvs.erase(vec_idx);
                    }
                }
            }
            if (field_removed && del_on_field_remove)
            {
                m_appCoppTable.del(key);
                bool key_present = true;
                while (key_present)
                {
                    vector<string> app_keys;
                    m_coppTable.getKeys(app_keys);
                    /* Loop until key is removed from app table */
                    auto del_key = std::find(app_keys.begin(), app_keys.end(), key);
                    if (del_key == app_keys.end())
                    {
                        key_present = false;
                    }
                    else
                    {
                        usleep(100);
                    }
                }
                modified_fvs = trap_group_fvs;
                break;
            }
        }
    }
}

void CoppMgr::doCoppGroupTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        auto fvs = kfvFieldsValues(t);
        string trap_ids;
        vector<FieldValueTuple> modified_fvs;

        if (op == SET_COMMAND)
        {
            if (g_copp_init_set.find(key) != g_copp_init_set.end())
            {
                g_copp_init_set.erase(key);
                it = consumer.m_toSync.erase(it);
                continue;
            }
            if (coppGroupHasRestrictedFields (fvs))
            {
                for (auto fv: fvs)
                {
                    m_coppGroupRestrictedMap[key][fvField(fv)] = fvValue(fv);
                }
            }
            if (checkIfTrapGroupFeaturePending(key))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            getTrapGroupTrapIds(key, trap_ids);
            if (!trap_ids.empty())
            {
                FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                fvs.push_back(fv);
            }

            coppGroupGetModifiedFvs(key, fvs, modified_fvs, false);
            if (!modified_fvs.empty())
            {
                m_appCoppTable.set(key, modified_fvs);
            }
            setCoppGroupStateOk(key);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("%s: DEL",key.c_str());
            m_appCoppTable.del(key);

            /* If the COPP group was part of init config, it needs to be recreated
             * with field values from init. The configuration delete should just clear
             * the externally applied user configuration
             */
            if (m_coppGroupInitCfg.find(key) != m_coppGroupInitCfg.end())
            {
                std::vector<FieldValueTuple> fvs = m_coppGroupInitCfg[key];
                if (m_coppGroupRestrictedMap.find(key) != m_coppGroupRestrictedMap.end())
                {
                    for (auto fv: fvs)
                    {
                        m_coppGroupRestrictedMap[key][fvField(fv)] = fvValue(fv);
                    }
                }
                if (!checkIfTrapGroupFeaturePending(key))
                {
                    getTrapGroupTrapIds(key, trap_ids);
                    if (!trap_ids.empty())
                    {
                        FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                        fvs.push_back(fv);
                    }
                    coppGroupGetModifiedFvs(key, fvs, modified_fvs, false);
                    if (!modified_fvs.empty())
                    {
                        m_appCoppTable.set(key, modified_fvs);
                    }
                }
            }
            else
            {
                if (m_coppGroupRestrictedMap.find(key) != m_coppGroupRestrictedMap.end())
                {
                    m_coppGroupRestrictedMap.erase(key);
                }
            }
            delCoppGroupStateOk(key);
        }
        it = consumer.m_toSync.erase(it);
    }
}

void CoppMgr::doFeatureTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fvs;
        string trap_ids;

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "status")
                {
                    bool status = false;
					if (fvValue(i) == "enabled")
                    {
                        status = true;
                    }
                    setFeatureTrapIdsStatus(key, status);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            setFeatureTrapIdsStatus(key, false);
        }
        it = consumer.m_toSync.erase(it);
    }
}

void CoppMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto table = consumer.getTableName();

    if (table == CFG_COPP_TRAP_TABLE_NAME)
    {
        doCoppTrapTask(consumer);
    }
    else if (table == CFG_COPP_GROUP_TABLE_NAME)
    {
        doCoppGroupTask(consumer);
    }
    else if (table == CFG_FEATURE_TABLE_NAME)
    {
        doFeatureTask(consumer);
    }
}
