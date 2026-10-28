#include <algorithm>
#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "vrfmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include "warm_restart.h"

#define VRF_TABLE_START 1001
#define VRF_TABLE_END 5097
#define TABLE_LOCAL_PREF 1001 // after l3mdev-table
#define MGMT_VRF_TABLE_ID 6000
#define MGMT_VRF          "mgmt"
#define DEFAULT_VRF       "default"

using namespace swss;

VrfMgr::VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_appVrfTableProducer(appDb, APP_VRF_TABLE_NAME),
        m_appVnetTableProducer(appDb, APP_VNET_TABLE_NAME),
        m_appVxlanVrfTableProducer(appDb, APP_VXLAN_VRF_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateVrfObjectTable(stateDb, STATE_VRF_OBJECT_TABLE_NAME),
        m_cfgVrfTable(cfgDb, CFG_VRF_TABLE_NAME),
        m_cfgVxlanTunnelMapTable(cfgDb, CFG_VXLAN_TUNNEL_MAP_TABLE_NAME)
{
    for (uint32_t i = VRF_TABLE_START; i < VRF_TABLE_END; i++)
    {
        m_freeTables.emplace(i);
    }

    /* Get existing VRFs from Linux */
    stringstream cmd;
    string res;

    cmd << IP_CMD << " -d link show type vrf";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    enum IpShowRowType
    {
        LINK_ROW,
        MAC_ROW,
        DETAILS_ROW,
    };

    string vrfName;
    uint32_t table;
    IpShowRowType rowType = LINK_ROW;
    const auto& rows = tokenize(res, '\n');
    for (const auto& row : rows)
    {
        const auto& items = tokenize(row, ' ');
        switch(rowType)
        {
            case LINK_ROW:
                vrfName = items[1];
                vrfName.pop_back();
                rowType = MAC_ROW;
                break;
            case MAC_ROW:
                rowType = DETAILS_ROW;
                break;
            case DETAILS_ROW:
                if (WarmStart::isWarmStart())
                {
                    table = static_cast<uint32_t>(stoul(items[6]));
                    m_vrfTableMap[vrfName] = table;
                    m_freeTables.erase(table);
                }
                else
                {
                    // No deletion of mgmt table from kernel
                    if (vrfName.compare("mgmt") == 0)
                    { 
                        SWSS_LOG_NOTICE("Skipping remove vrf device %s", vrfName.c_str());
                        rowType = LINK_ROW;
                        break;
                    }

                    SWSS_LOG_NOTICE("Remove vrf device %s", vrfName.c_str());
                    cmd.str("");
                    cmd.clear();
                    cmd << IP_CMD << " link del " << vrfName;
                    int ret = swss::exec(cmd.str(), res);
                    if (ret)
                    {
                        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
                    }
                }
                rowType = LINK_ROW;
                break;
        }
    }

    cmd.str("");
    cmd.clear();
    cmd << IP_CMD << " rule | grep '^0:'";
    if (swss::exec(cmd.str(), res) == 0)
    {
        cmd.str("");
        cmd.clear();
        cmd << IP_CMD << " rule add pref " << TABLE_LOCAL_PREF << " table local && " << IP_CMD << " rule del pref 0 && "
            << IP_CMD << " -6 rule add pref " << TABLE_LOCAL_PREF << " table local && " << IP_CMD << " -6 rule del pref 0";
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
    }

    if (!WarmStart::isWarmStart())
    {
        WarmStart::setWarmStartState("vrfmgrd", WarmStart::WSDISABLED);
    }
}

uint32_t VrfMgr::getFreeTable(void)
{
    SWSS_LOG_ENTER();

    if (m_freeTables.empty())
    {
        return 0;
    }

    uint32_t table = *m_freeTables.begin();
    m_freeTables.erase(table);

    return table;
}

void VrfMgr::recycleTable(uint32_t table)
{
    SWSS_LOG_ENTER();

    m_freeTables.emplace(table);
}

bool VrfMgr::delLink(const string& vrfName)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    if (m_vrfTableMap.find(vrfName) == m_vrfTableMap.end())
    {
        return false;
    }

    if (vrfName == MGMT_VRF)
    {
        recycleTable(m_vrfTableMap[vrfName]);
        m_vrfTableMap.erase(vrfName);
        return true;
    }

    cmd << IP_CMD << " link del " << shellquote(vrfName);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    recycleTable(m_vrfTableMap[vrfName]);
    m_vrfTableMap.erase(vrfName);

    return true;
}

bool VrfMgr::setLink(const string& vrfName)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    if (m_vrfTableMap.find(vrfName) != m_vrfTableMap.end())
    {
        return true;
    }
    
    if (vrfName == MGMT_VRF)
    {
        // Mgmt VRF is initialised as part of hostcfgd, 
        // just return the reserved table_id for mgmt VRF from here.
        uint32_t table_id = MGMT_VRF_TABLE_ID;
        m_vrfTableMap.emplace(vrfName, table_id);
        return true;
    }

    uint32_t table = getFreeTable();
    if (table == 0)
    {
        return false;
    }

    cmd << IP_CMD << " link add " << shellquote(vrfName) << " type vrf table " << table;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    m_vrfTableMap.emplace(vrfName, table);

    cmd.str("");
    cmd.clear();
    cmd << IP_CMD << " link set " << shellquote(vrfName) << " up";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return true;
}

bool VrfMgr::isVrfObjExist(const string& vrfName)
{
    vector<FieldValueTuple> temp;

    if (m_stateVrfObjectTable.get(vrfName, temp))
    {
        SWSS_LOG_DEBUG("Vrf %s object exist", vrfName.c_str());
        return true;
    }

    return false;
}

void VrfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto vrfName = kfvKey(t);

        string op = kfvOp(t);
        if (consumer.getTableName() == CFG_VXLAN_TUNNEL_MAP_TABLE_NAME)
        {
            const size_t delimiter_pos = vrfName.find(config_db_key_delimiter);
            if (delimiter_pos == string::npos)
            {
                SWSS_LOG_ERROR("Invalid VXLAN tunnel map key %s", vrfName.c_str());
            }
            else
            {
                const string tunnel_name = vrfName.substr(0, delimiter_pos);
                if (op == SET_COMMAND)
                {
                    string vni;
                    for (const auto& field_value : kfvFieldsValues(t))
                    {
                        if (fvField(field_value) == "vni")
                        {
                            vni = fvValue(field_value);
                            break;
                        }
                    }

                    if (vni.empty())
                    {
                        /*
                         * CONFIG_DB notifications can contain only the fields
                         * changed by a partial SET. Read this one row to
                         * distinguish an unchanged VNI from removal of the VNI
                         * field; never scan the table.
                         */
                        vector<FieldValueTuple> config_fields;
                        if (m_cfgVxlanTunnelMapTable.get(vrfName, config_fields))
                        {
                            for (const auto& field_value : config_fields)
                            {
                                if (fvField(field_value) == "vni")
                                {
                                    vni = fvValue(field_value);
                                    break;
                                }
                            }
                        }
                    }

                    if (vni.empty())
                    {
                        VxlanTunnelMapCacheEntry old_entry;
                        if (removeVxlanTunnelMapFromCache(vrfName, old_entry))
                        {
                            SWSS_LOG_NOTICE("VXLAN tunnel map %s no longer has a VNI; withdrawing cached dependency",
                                            vrfName.c_str());
                            syncVrfVxlanTableForVni(old_entry.tunnel_name, old_entry.vni);
                        }
                        else
                        {
                            SWSS_LOG_ERROR("VXLAN tunnel map %s has no VNI", vrfName.c_str());
                        }
                    }
                    else
                    {
                        const auto cached = m_vxlanTunnelMapCache.find(vrfName);
                        if ((cached != m_vxlanTunnelMapCache.end()) &&
                            ((cached->second.tunnel_name != tunnel_name) || (cached->second.vni != vni)))
                        {
                            VxlanTunnelMapCacheEntry old_entry;
                            removeVxlanTunnelMapFromCache(vrfName, old_entry);
                            syncVrfVxlanTableForVni(old_entry.tunnel_name, old_entry.vni);
                        }

                        if (m_vxlanTunnelMapCache.find(vrfName) == m_vxlanTunnelMapCache.end())
                        {
                            addVxlanTunnelMapToCache(vrfName, tunnel_name, vni);
                        }
                        syncVrfVxlanTableForVni(tunnel_name, vni);
                    }
                }
                else if (op == DEL_COMMAND)
                {
                    VxlanTunnelMapCacheEntry old_entry;
                    if (removeVxlanTunnelMapFromCache(vrfName, old_entry))
                    {
                        syncVrfVxlanTableForVni(old_entry.tunnel_name, old_entry.vni);
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown VXLAN tunnel map operation %s for %s",
                                   op.c_str(), vrfName.c_str());
                }
            }
            it = consumer.m_toSync.erase(it);
            continue;
        }

        // Mgmt VRF table event handling for in-band management
        if (consumer.getTableName() == CFG_MGMT_VRF_CONFIG_TABLE_NAME)
        {
            SWSS_LOG_DEBUG("Event for mgmt VRF op %s", op.c_str());
            if (op == SET_COMMAND) 
            {
                bool in_band_mgmt_enabled = false;
                bool mgmt_vrf_enabled = false;
                for (auto i : kfvFieldsValues(t))
                {
                    if (fvField(i) == "mgmtVrfEnabled")
                    {
                        if (fvValue(i) == "true")
                        {
                            mgmt_vrf_enabled = true;
                        }
                        SWSS_LOG_DEBUG("Event for mgmt VRF table mgmt_vrf_enabled is set val:%s", fvValue(i).c_str());
                    }
                    else if (fvField(i) == "in_band_mgmt_enabled")
                    {
                        if (fvValue(i) == "true")
                        {
                            in_band_mgmt_enabled = true;
                        }
                        SWSS_LOG_DEBUG("Event for mgmt VRF table in_band_mgmt_enabled is set val:%s", fvValue(i).c_str());
                    }
                }
                // If mgmt VRF is not enabled or in-band-mgmt is not enabled delete the in-band-mgmt 
                // related VRF table map information
                if ((op == SET_COMMAND) && ((mgmt_vrf_enabled == false) || (in_band_mgmt_enabled == false)))
                {
                    op = DEL_COMMAND;
                }
            }
            vrfName = MGMT_VRF;
            if (((op == DEL_COMMAND) && (m_vrfTableMap.find(vrfName) == m_vrfTableMap.end())) ||
                    ((op == SET_COMMAND) && (m_vrfTableMap.find(vrfName) != m_vrfTableMap.end())))
            {
                // If the mgmt VRF is not populated already, return
                it = consumer.m_toSync.erase(it);
                continue;
            }
            SWSS_LOG_DEBUG("Event for mgmt VRF op %s", op.c_str());
        }
        SWSS_LOG_DEBUG("Event for table %s vrf netdev %s id %s", consumer.getTableName().c_str(), vrfName.c_str(), op.c_str());
        if (op == SET_COMMAND)
        {
            if (consumer.getTableName() == CFG_VXLAN_EVPN_NVO_TABLE_NAME)
            {
                doVrfEvpnNvoAddTask(t);
            }
            else
            {
                KeyOpFieldsValuesTuple effective_t = t;
                bool vni_in_event = false;
                for (const auto& field_value : kfvFieldsValues(effective_t))
                {
                    if (fvField(field_value) == "vni")
                    {
                        vni_in_event = true;
                        break;
                    }
                }

                /*
                 * CONFIG_DB notifications can contain only the fields changed
                 * by a partial SET.  Recover an unchanged VNI from the current
                 * row so that updating another VRF property does not remove the
                 * mapping.  If the row itself no longer has a VNI, leave it
                 * absent: doVrfVxlanTableCreateTask() then performs the explicit
                 * unmap transition.
                 */
                if ((consumer.getTableName() == CFG_VRF_TABLE_NAME) && !vni_in_event)
                {
                    vector<FieldValueTuple> config_fields;
                    if (m_cfgVrfTable.get(vrfName, config_fields))
                    {
                        for (const auto& field_value : config_fields)
                        {
                            if (fvField(field_value) == "vni")
                            {
                                kfvFieldsValues(effective_t).emplace_back("vni", fvValue(field_value));
                                vni_in_event = true;
                                break;
                            }
                        }
                    }
                }

                if ((consumer.getTableName() == CFG_VRF_TABLE_NAME) ||
                    (consumer.getTableName() == CFG_MGMT_VRF_CONFIG_TABLE_NAME))
                {
                    uint32_t validated_vni = 0;
                    string validated_s_vni;
                    if (!validateVrfVniConfig(effective_t, validated_vni, validated_s_vni))
                    {
                        SWSS_LOG_ERROR("Rejected VRF VNI configuration for %s", vrfName.c_str());
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }

                if (vrfName != DEFAULT_VRF)
                {
                    if (!setLink(vrfName))
                    {
                        SWSS_LOG_ERROR("Failed to create vrf netdev %s", vrfName.c_str());
                    }
                    SWSS_LOG_NOTICE("Created vrf netdev %s", vrfName.c_str());
                }
                else
                {
                    SWSS_LOG_NOTICE("Skipping VRF netdev creation for default VRF");
                }

                bool status = true;
                vector<FieldValueTuple> fvVector;
                fvVector.emplace_back("state", "ok");
                m_stateVrfTable.set(vrfName, fvVector);

                if ((consumer.getTableName() == CFG_VRF_TABLE_NAME) ||
                    (consumer.getTableName() == CFG_MGMT_VRF_CONFIG_TABLE_NAME))
                {
                    const uint32_t old_vni = getVRFmappedVNI(vrfName);
                    status = doVrfVxlanTableCreateTask(effective_t);
                    if (status == false)
                    {
                        SWSS_LOG_ERROR("VRF VNI Map Config Failed");
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                    auto app_fields = kfvFieldsValues(effective_t);
                    if (!vni_in_event && (old_vni != 0) && (getVRFmappedVNI(vrfName) == 0))
                    {
                        /*
                         * ProducerStateTable SETs merge fields.  Publish the
                         * lifecycle value explicitly so APP_DB and VRFOrch do
                         * not retain the removed VNI from an earlier update.
                         */
                        app_fields.emplace_back("vni", "0");
                    }
                    m_appVrfTableProducer.set(vrfName, app_fields);
                }
                else
                {
                    m_appVnetTableProducer.set(vrfName, kfvFieldsValues(t));
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            /*
             * Delay delLink until vrf object deleted in orchagent to ensure fpmsyncd can get vrf ifname.
             * Now state VRF_TABLE|Vrf represent vrf exist in appDB, if it exist vrf device is always effective.
             * VRFOrch add/del state VRF_OBJECT_TABLE|Vrf to represent object existence. VNETOrch is not do so now.
             */
            if (consumer.getTableName() == CFG_VXLAN_EVPN_NVO_TABLE_NAME)
            {
                doVrfEvpnNvoDelTask (t);
            }
            else if ((consumer.getTableName() == CFG_VRF_TABLE_NAME) ||
                     (consumer.getTableName() == CFG_MGMT_VRF_CONFIG_TABLE_NAME))
            {
                vector<FieldValueTuple> temp;

                if (m_stateVrfTable.get(vrfName, temp))
                {
                    /* VRFOrch add delay so wait */
                    if (!isVrfObjExist(vrfName))
                    {
                        it++;
                        continue;
                    }

                    doVrfVxlanTableRemoveTask (t);
                    m_appVrfTableProducer.del(vrfName);
                    m_stateVrfTable.del(vrfName);
                }

                if (isVrfObjExist(vrfName))
                {
                    it++;
                    continue;
                }
            }
            else
            {
                m_appVnetTableProducer.del(vrfName);
                m_stateVrfTable.del(vrfName);
            }

            if (consumer.getTableName() != CFG_VXLAN_EVPN_NVO_TABLE_NAME)
            {
                if (vrfName != DEFAULT_VRF)
                {
                    if (!delLink(vrfName))
                    {
                        SWSS_LOG_ERROR("Failed to remove vrf netdev %s", vrfName.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_NOTICE("Skipping VRF netdev deletion for default VRF");
                }
            }

            SWSS_LOG_NOTICE("Removed vrf netdev %s", vrfName.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool VrfMgr::doVrfEvpnNvoAddTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    auto nvo_name = kfvKey(t);
    string tunnel_name = "";
    const vector<FieldValueTuple>& data = kfvFieldsValues(t);
    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "source_vtep")
        {
            tunnel_name = value;
        }
    }

    string old_tunnel_name;
    const auto old_nvo = m_evpnVxlanTunnel.find(nvo_name);
    if (old_nvo != m_evpnVxlanTunnel.end())
    {
        old_tunnel_name = old_nvo->second;
    }

    m_evpnVxlanTunnel[nvo_name] = tunnel_name;
    if (!old_tunnel_name.empty() && old_tunnel_name != tunnel_name)
    {
        syncVrfVxlanTableForTunnel(old_tunnel_name);
    }
    syncVrfVxlanTableForTunnel(tunnel_name);

    SWSS_LOG_INFO("Added evpn nvo %s with tunnel %s", nvo_name.c_str(), tunnel_name.c_str());
    return true;
}

bool VrfMgr::doVrfEvpnNvoDelTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    auto nvo_name = kfvKey(t);

    auto it = m_evpnVxlanTunnel.find(nvo_name);
    if (it != m_evpnVxlanTunnel.end())
    {
        string tunnel_name = it->second;
        m_evpnVxlanTunnel.erase(it);
        syncVrfVxlanTableForTunnel(tunnel_name);

        SWSS_LOG_INFO("Removed evpn nvo %s with tunnel %s", nvo_name.c_str(), tunnel_name.c_str());
    }
    return true;
}

bool VrfMgr::validateVrfVniConfig(const KeyOpFieldsValuesTuple & t, uint32_t& vni, string& s_vni)
{
    constexpr uint32_t MAX_VNI = (1U << 24) - 1;
    const string& vrf_name = kfvKey(t);
    vni = 0;
    s_vni.clear();

    for (const auto& field_value : kfvFieldsValues(t))
    {
        if (fvField(field_value) != "vni")
        {
            continue;
        }

        s_vni = fvValue(field_value);
        try
        {
            size_t parsed = 0;
            const auto value = stoul(s_vni, &parsed);
            if ((parsed != s_vni.size()) || (value > MAX_VNI))
            {
                SWSS_LOG_ERROR("Invalid VNI '%s' for VRF %s", s_vni.c_str(), vrf_name.c_str());
                return false;
            }
            vni = static_cast<uint32_t>(value);
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Invalid VNI '%s' for VRF %s: %s", s_vni.c_str(), vrf_name.c_str(), e.what());
            return false;
        }
        break;
    }

    const uint32_t old_vni = getVRFmappedVNI(vrf_name);
    if ((old_vni != 0) && (vni != 0) && (vni != old_vni))
    {
        SWSS_LOG_ERROR("Live VNI reconfiguration for VRF %s is not supported: old %u, requested %u",
                       vrf_name.c_str(), old_vni, vni);
        return false;
    }

    if (vni != 0)
    {
        for (const auto& mapping : m_vrfVniMapTable)
        {
            if ((mapping.first != vrf_name) && (mapping.second == vni))
            {
                SWSS_LOG_ERROR("VNI %u is already mapped to VRF %s", vni, mapping.first.c_str());
                return false;
            }
        }
    }

    return true;
}

bool VrfMgr::doVrfVxlanTableCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    auto vrf_name = kfvKey(t);
    uint32_t vni = 0, old_vni = 0;
    string s_vni = "";
    bool add = true;

    if (!validateVrfVniConfig(t, vni, s_vni))
    {
        return false;
    }

    old_vni = getVRFmappedVNI(vrf_name);
    SWSS_LOG_INFO("VRF VNI map update vrf %s, vni %d, old_vni %d", vrf_name.c_str(), vni, old_vni);

    if (vni == old_vni)
    {
        return true;
    }

    if (vni == 0)
    {
        return doVrfVxlanTableRemoveTask(t);
    }

    m_vrfVniMapTable[vrf_name] = vni;
    m_vniVrfMapTable[s_vni] = vrf_name;
    SWSS_LOG_INFO("VRF VNI map update vrf %s, s_vni %s, add %d", vrf_name.c_str(), s_vni.c_str(), add);
    doVrfVxlanTableUpdate(vrf_name, s_vni, add);
    return true;
}

bool VrfMgr::doVrfVxlanTableRemoveTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    auto vrf_name = kfvKey(t);
    uint32_t vni = 0;
    string s_vni = "";

    vni = getVRFmappedVNI(vrf_name);
    SWSS_LOG_INFO("VRF VNI map remove vrf %s, vni %d", vrf_name.c_str(), vni);
    if (vni != 0)
    {
        s_vni = to_string(vni);
        doVrfVxlanTableUpdate(vrf_name, s_vni, false);
        m_vniVrfMapTable.erase(s_vni);
        m_vrfVniMapTable.erase(vrf_name);
    }

    return true;
}

bool VrfMgr::doVrfVxlanTableUpdate(const string& vrf_name, const string& vni, bool add)
{
    SWSS_LOG_ENTER();

    if (add && m_evpnVxlanTunnel.empty())
    {
        SWSS_LOG_INFO("NVO Tunnel not present. vrf %s, vni %s, add %d", vrf_name.c_str(), vni.c_str(), add);
        return false;
    }

    std::vector<std::string> tunnels = getVxlanTunnelsForVni(vni);
    if (tunnels.empty())
    {
        SWSS_LOG_INFO("No VXLAN tunnels found with VNI %s for VRF %s", vni.c_str(), vrf_name.c_str());
        return false;
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple v1("vni", vni);
    FieldValueTuple v2("vrf", vrf_name);
    fvVector.push_back(v1);
    fvVector.push_back(v2);

    for (const auto& tunnel_name : tunnels)
    {
        const bool active_nvo_tunnel = std::any_of(
            m_evpnVxlanTunnel.begin(), m_evpnVxlanTunnel.end(),
            [&tunnel_name](const auto& nvo) { return nvo.second == tunnel_name; });
        if (add && !active_nvo_tunnel)
        {
            continue;
        }

        string key = tunnel_name + ":" + "evpn_map_" + vni + "_" + vrf_name;

        SWSS_LOG_INFO("VRF VNI map table update for tunnel %s, vrf %s, vni %s, add %d", 
                      tunnel_name.c_str(), vrf_name.c_str(), vni.c_str(), add);

        if (add)
        {
            if (m_publishedVrfVxlanMappings.insert(key).second)
            {
                m_appVxlanVrfTableProducer.set(key, fvVector);
            }
        }
        else if (m_publishedVrfVxlanMappings.erase(key) != 0)
        {
            m_appVxlanVrfTableProducer.del(key);
        }
    }

    return true;
}

void VrfMgr::syncVrfVxlanTableForTunnel(const string& tunnel_name)
{
    SWSS_LOG_ENTER();

    for (const auto& vni_tunnels : m_vniTunnelRefCount)
    {
        if (vni_tunnels.second.find(tunnel_name) != vni_tunnels.second.end())
        {
            syncVrfVxlanTableForVni(tunnel_name, vni_tunnels.first);
        }
    }
}

void VrfMgr::syncVrfVxlanTableForVni(const string& tunnel_name, const string& vni)
{
    SWSS_LOG_ENTER();

    const auto vrf = m_vniVrfMapTable.find(vni);
    if (vrf == m_vniVrfMapTable.end())
    {
        return;
    }

    const bool active_nvo_tunnel = std::any_of(
        m_evpnVxlanTunnel.begin(), m_evpnVxlanTunnel.end(),
        [&tunnel_name](const auto& nvo) { return nvo.second == tunnel_name; });

    const auto vni_tunnels = m_vniTunnelRefCount.find(vni);
    const bool vni_mapped_to_tunnel =
        (vni_tunnels != m_vniTunnelRefCount.end()) &&
        (vni_tunnels->second.find(tunnel_name) != vni_tunnels->second.end());
    const bool should_exist = active_nvo_tunnel && vni_mapped_to_tunnel;
    const string key = tunnel_name + ":" + "evpn_map_" + vni + "_" + vrf->second;

    SWSS_LOG_INFO("Reconcile VRF VNI map vrf %s, vni %s, tunnel %s, present %d",
                  vrf->second.c_str(), vni.c_str(), tunnel_name.c_str(), should_exist);
    if (should_exist)
    {
        if (m_publishedVrfVxlanMappings.insert(key).second)
        {
            std::vector<FieldValueTuple> fvVector;
            fvVector.emplace_back("vni", vni);
            fvVector.emplace_back("vrf", vrf->second);
            m_appVxlanVrfTableProducer.set(key, fvVector);
        }
    }
    else if (m_publishedVrfVxlanMappings.erase(key) != 0)
    {
        m_appVxlanVrfTableProducer.del(key);
    }
}

void VrfMgr::addVxlanTunnelMapToCache(const string& key, const string& tunnel_name, const string& vni)
{
    m_vxlanTunnelMapCache[key] = {tunnel_name, vni};
    ++m_vniTunnelRefCount[vni][tunnel_name];
}

bool VrfMgr::removeVxlanTunnelMapFromCache(const string& key, VxlanTunnelMapCacheEntry& entry)
{
    const auto cached = m_vxlanTunnelMapCache.find(key);
    if (cached == m_vxlanTunnelMapCache.end())
    {
        return false;
    }

    entry = cached->second;
    m_vxlanTunnelMapCache.erase(cached);

    auto vni_tunnels = m_vniTunnelRefCount.find(entry.vni);
    if (vni_tunnels != m_vniTunnelRefCount.end())
    {
        auto tunnel = vni_tunnels->second.find(entry.tunnel_name);
        if (tunnel != vni_tunnels->second.end())
        {
            if (--tunnel->second == 0)
            {
                vni_tunnels->second.erase(tunnel);
            }
        }
        if (vni_tunnels->second.empty())
        {
            m_vniTunnelRefCount.erase(vni_tunnels);
        }
    }

    return true;
}

uint32_t VrfMgr::getVRFmappedVNI(const std::string& vrf_name)
{
    if (m_vrfVniMapTable.find(vrf_name) != std::end(m_vrfVniMapTable))
    {
        return m_vrfVniMapTable.at(vrf_name);
    }
    else
    {
        return 0;
    }
}

std::vector<std::string> VrfMgr::getVxlanTunnelsForVni(const std::string& vni)
{
    SWSS_LOG_ENTER();

    std::vector<std::string> tunnels;
    const auto vni_tunnels = m_vniTunnelRefCount.find(vni);
    if (vni_tunnels != m_vniTunnelRefCount.end())
    {
        for (const auto& tunnel : vni_tunnels->second)
        {
            tunnels.push_back(tunnel.first);
            SWSS_LOG_INFO("Found cached tunnel %s with VNI %s", tunnel.first.c_str(), vni.c_str());
        }
    }

    return tunnels;
}
