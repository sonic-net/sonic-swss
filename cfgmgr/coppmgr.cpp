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

void CoppMgr::parseInitFile(void)
{
    std::ifstream ifs(COPP_INIT_FILE);
    if (ifs.fail())
    {
        SWSS_LOG_ERROR("COPP init file %s not found", COPP_INIT_FILE);
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
bool CoppMgr::checkTrapGroupPending(string trap_group_name)
{
    bool traps_present = false;
    for (auto it: m_coppTrapIdTrapGroupMap)
    {
        if (it.second == trap_group_name)
        {
            traps_present = true;
            /* At least one trap should be enabled to install the trap group
             */
            if (!isTrapIdDisabled(it.first))
            {
                return false;
            }
        }
    }
    return traps_present;
}

/* Feature name and CoPP Trap table name must match */
void CoppMgr::setFeatureTrapIdsStatus(string feature, bool enable)
{
    bool disabled_trap = (m_coppDisabledTraps.find(feature)  != m_coppDisabledTraps.end());

    if ((enable && !disabled_trap) || (!enable && disabled_trap))
    {
        return;
    }

    if (m_coppTrapConfMap.find(feature) == m_coppTrapConfMap.end())
    {
        if (!enable)
        {
            m_coppDisabledTraps.insert(feature);
        }
        return;
    }
    string trap_group = m_coppTrapConfMap[feature].trap_group;
    bool prev_group_state = checkTrapGroupPending(trap_group);

    if (!enable)
    {
        m_coppDisabledTraps.insert(feature);
    }
    else
    {
        m_coppDisabledTraps.erase(feature);
    }

    /* Trap group moved to pending state when feature is disabled. Remove trap group
    */
    if (checkTrapGroupPending(trap_group) && !prev_group_state)
    {
        m_appCoppTable.del(trap_group);
        delCoppGroupStateOk(trap_group);
        return;
    }
    vector<FieldValueTuple> fvs;
    string trap_ids;

    /* Trap group moved from pending state to enabled state
     * Hence install include all fields to create the group
     */
    if (prev_group_state && !checkTrapGroupPending(trap_group))
    {
        for (auto i : m_coppGroupFvs[trap_group])
        {
            FieldValueTuple fv(i.first, i.second);
            fvs.push_back(fv);
        }
    }
    getTrapGroupTrapIds(trap_group, trap_ids);
    if (!trap_ids.empty())
    {
        FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
        fvs.push_back(fv);
    }
    if (!fvs.empty())
    {
        m_appCoppTable.set(trap_group, fvs);
        setCoppGroupStateOk(trap_group);
    }
}

bool CoppMgr::isTrapIdDisabled(string trap_id)
{
    for (auto &m: m_coppDisabledTraps)
    {
        if (m_coppTrapConfMap.find(m) == m_coppTrapConfMap.end())
        {
            continue;
        }
        vector<string> trap_id_list;

        trap_id_list = tokenize(m_coppTrapConfMap[m].trap_ids, list_item_delimiter);
        if(std::find(trap_id_list.begin(), trap_id_list.end(), trap_id) != trap_id_list.end())
        {
            return true;
        }

    }
    return false;
}
void CoppMgr::mergeConfig(CoppCfg &init_cfg, CoppCfg &m_cfg, std::vector<std::string> &cfg_keys, Table &cfgTable)
{
    /* Read the init configuration first. If the same key is present in
     * user configuration, override the init fields with user fields
     */
    for (auto i : init_cfg)
    {
        std::vector<FieldValueTuple> init_fvs = i.second;
        std::vector<FieldValueTuple> merged_fvs;
        auto key = std::find(cfg_keys.begin(), cfg_keys.end(), i.first);
        bool null_cfg = false;
        if (key != cfg_keys.end())
        {
            std::vector<FieldValueTuple> cfg_fvs;
            cfgTable.get(i.first, cfg_fvs);

            merged_fvs = cfg_fvs;
            for (auto it1: init_fvs)
            {
                bool field_found = false;
                for (auto it2: cfg_fvs)
                {
                    if(fvField(it2) == "NULL")
                    {
                        SWSS_LOG_DEBUG("Ignoring create for key %s",i.first.c_str());
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
                    merged_fvs.push_back(it1);
                }
            }
            if (!null_cfg)
            {
                m_cfg[i.first] = merged_fvs;
            }
        }
        else
        {
            m_cfg[i.first] = init_cfg[i.first];
        }
    }

    /* Read the user configuration keys that were not present in
     * init configuration.
     */
    for (auto i : cfg_keys)
    {
        if(init_cfg.find(i) == init_cfg.end())
        {
            std::vector<FieldValueTuple> cfg_fvs;
            cfgTable.get(i, cfg_fvs);
            m_cfg[i] = cfg_fvs;
        }
    }
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


    for (auto i: feature_keys)
    {
        std::vector<FieldValueTuple> feature_fvs;
        m_cfgFeatureTable.get(i, feature_fvs);

        for (auto j: feature_fvs)
        {
            if (fvField(j) == "status" && fvValue(j) == "disabled")
            {
                m_coppDisabledTraps.insert(i);
            }
        }
    }

    mergeConfig(m_coppTrapInitCfg, trap_cfg, trap_cfg_keys, m_cfgCoppTrapTable);

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
            setCoppTrapStateOk(i.first);
        }
    }

    mergeConfig(m_coppGroupInitCfg, group_cfg, group_cfg_keys, m_cfgCoppGroupTable);

    for (auto i: group_cfg)
    {
        string trap_ids;
        vector<FieldValueTuple> trap_group_fvs = i.second;

        for (auto fv: trap_group_fvs)
        {
            m_coppGroupFvs[i.first][fvField(fv)]= fvValue(fv);
        }
        if (checkTrapGroupPending(i.first))
        {
            continue;
        }

        getTrapGroupTrapIds(i.first, trap_ids);
        if (!trap_ids.empty())
        {
            FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
            trap_group_fvs.push_back(fv);
        }

        if (!trap_group_fvs.empty())
        {
            m_appCoppTable.set(i.first, trap_group_fvs);
        }
        setCoppGroupStateOk(i.first);
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
            if (isTrapIdDisabled(it.first))
            {
                continue;
            }
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
                    setCoppTrapStateOk(key);
                    getTrapGroupTrapIds(m_coppTrapConfMap[key].trap_group, trap_ids);
                    fvs.clear();
                    FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                    fvs.push_back(fv);
                    if (!checkTrapGroupPending(m_coppTrapConfMap[key].trap_group))
                    {
                        m_appCoppTable.set(m_coppTrapConfMap[key].trap_group, fvs);
                    }
                    setCoppGroupStateOk(m_coppTrapConfMap[key].trap_group);
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
            if (!checkTrapGroupPending(trap_group))
            {
                m_appCoppTable.set(trap_group, fvs);
                setCoppGroupStateOk(trap_group);
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
                if (!checkTrapGroupPending(m_coppTrapConfMap[key].trap_group))
                {
                    m_appCoppTable.set(m_coppTrapConfMap[key].trap_group, fvs);
                    setCoppGroupStateOk(m_coppTrapConfMap[key].trap_group);
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
                if (!checkTrapGroupPending(m_coppTrapConfMap[key].trap_group))
                {
                    m_appCoppTable.set(m_coppTrapConfMap[key].trap_group, fvs);
                    setCoppGroupStateOk(m_coppTrapConfMap[key].trap_group);
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
                if (!checkTrapGroupPending(trap_group))
                {
                    m_appCoppTable.set(trap_group, g_fvs);
                    setCoppGroupStateOk(trap_group);
                }
                m_coppTrapConfMap[key].trap_group = trap_group;
                m_coppTrapConfMap[key].trap_ids = trap_ids;
                setCoppTrapStateOk(key);
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}

/* This API is used to fetch only the modified configurations
 */
void CoppMgr::coppGroupGetModifiedFvs(string key, vector<FieldValueTuple> &trap_group_fvs,
                                      vector<FieldValueTuple> &modified_fvs)
{
    modified_fvs = trap_group_fvs;

    if (m_coppGroupFvs.find(key) == m_coppGroupFvs.end())
    {
        return;
    }

    for (auto fv: trap_group_fvs)
    {
        if(fvField(fv) == COPP_TRAP_ID_LIST_FIELD ||
            m_coppGroupFvs[key].find(fvField(fv)) == m_coppGroupFvs[key].end())
        {
            continue;
        }

        if (m_coppGroupFvs[key][fvField(fv)] == fvValue(fv))
        {
            auto vec_idx = std::find(modified_fvs.begin(), modified_fvs.end(), fv);
            modified_fvs.erase(vec_idx);
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

            getTrapGroupTrapIds(key, trap_ids);
            if (!trap_ids.empty())
            {
                FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                fvs.push_back(fv);
            }

            coppGroupGetModifiedFvs(key, fvs, modified_fvs);
            if (!checkTrapGroupPending(key))
            {
                if (!modified_fvs.empty())
                {
                    m_appCoppTable.set(key, modified_fvs);
                    setCoppGroupStateOk(key);
                }
            }
            for (auto fv: fvs)
            {
                if(fvField(fv) != COPP_TRAP_ID_LIST_FIELD)
                {
                    m_coppGroupFvs[key][fvField(fv)] = fvValue(fv);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("%s: DEL",key.c_str());
            if (!checkTrapGroupPending(key))
            {
                m_appCoppTable.del(key);
                delCoppGroupStateOk(key);
            }

            /* If the COPP group was part of init config, it needs to be recreated
             * with field values from init. The configuration delete should just clear
             * the externally applied user configuration
             */
            if (m_coppGroupInitCfg.find(key) != m_coppGroupInitCfg.end())
            {
                std::vector<FieldValueTuple> fvs = m_coppGroupInitCfg[key];
                for (auto fv: fvs)
                {
                    m_coppGroupFvs[key][fvField(fv)] = fvValue(fv);
                }
                if (!checkTrapGroupPending(key))
                {
                    getTrapGroupTrapIds(key, trap_ids);
                    if (!trap_ids.empty())
                    {
                        FieldValueTuple fv(COPP_TRAP_ID_LIST_FIELD, trap_ids);
                        fvs.push_back(fv);
                    }
                    m_appCoppTable.set(key, fvs);
                    setCoppGroupStateOk(key);
                }
            }
            else
            {
                m_coppGroupFvs.erase(key);
            }
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
