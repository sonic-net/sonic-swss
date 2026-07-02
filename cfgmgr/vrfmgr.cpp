#include <string.h>
#include <exception>
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

using namespace swss;

namespace
{

/* Matches the CFG table name generated from the companion YANG model. */
constexpr auto KERNEL_VRF_FALLBACK_CONFIG_TABLE = "KERNEL_VRF_FALLBACK";
constexpr auto KERNEL_VRF_FALLBACK_GLOBAL_KEY = "GLOBAL";
constexpr auto KERNEL_VRF_FALLBACK_STATUS_FIELD = "status";
constexpr auto KERNEL_VRF_FALLBACK_ENABLED = "enabled";
constexpr auto KERNEL_VRF_FALLBACK_DISABLED = "disabled";
constexpr auto KERNEL_VRF_SENTINEL_METRIC = "4278198272";

bool hasManagedKernelVrfSentinel(const string& routes)
{
    istringstream routeStream(routes);
    string line;

    while (getline(routeStream, line))
    {
        istringstream lineStream(line);
        vector<string> fields;
        string field;
        while (lineStream >> field)
        {
            fields.push_back(field);
        }
        if (fields.size() < 4 || fields[0] != "unreachable" || fields[1] != "default")
        {
            continue;
        }

        for (size_t i = 2; i + 1 < fields.size(); ++i)
        {
            if (fields[i] == "metric" && fields[i + 1] == KERNEL_VRF_SENTINEL_METRIC)
            {
                return true;
            }
        }
    }

    return false;
}

bool executeKernelVrfCommand(const string& command, string& result, int& rc)
{
    try
    {
        rc = swss::exec(command, result);
        return true;
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("Command '%s' failed: %s", command.c_str(), e.what());
        return false;
    }
}

} // namespace

VrfMgr::VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgKernelVrfFallbackTable(cfgDb, KERNEL_VRF_FALLBACK_CONFIG_TABLE),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateVrfObjectTable(stateDb, STATE_VRF_OBJECT_TABLE_NAME),
        m_appVrfTableProducer(appDb, APP_VRF_TABLE_NAME),
        m_appVnetTableProducer(appDb, APP_VNET_TABLE_NAME),
        m_appVxlanVrfTableProducer(appDb, APP_VXLAN_VRF_TABLE_NAME)
{
    loadKernelVrfFallbackState();

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

    reconcileAllKernelVrfs();
}

bool VrfMgr::parseKernelVrfFallbackState(const vector<FieldValueTuple>& values)
{
    for (const auto& value : values)
    {
        if (fvField(value) != KERNEL_VRF_FALLBACK_STATUS_FIELD)
        {
            continue;
        }

        if (fvValue(value) == KERNEL_VRF_FALLBACK_ENABLED)
        {
            return true;
        }
        if (fvValue(value) == KERNEL_VRF_FALLBACK_DISABLED)
        {
            return false;
        }

        SWSS_LOG_ERROR("Invalid %s value '%s' in %s|%s; using disabled",
                       KERNEL_VRF_FALLBACK_STATUS_FIELD, fvValue(value).c_str(),
                       KERNEL_VRF_FALLBACK_CONFIG_TABLE, KERNEL_VRF_FALLBACK_GLOBAL_KEY);
        return false;
    }

    SWSS_LOG_ERROR("Missing %s field in %s|%s; using disabled",
                   KERNEL_VRF_FALLBACK_STATUS_FIELD, KERNEL_VRF_FALLBACK_CONFIG_TABLE,
                   KERNEL_VRF_FALLBACK_GLOBAL_KEY);
    return false;
}

void VrfMgr::loadKernelVrfFallbackState()
{
    vector<FieldValueTuple> values;
    if (!m_cfgKernelVrfFallbackTable.get(KERNEL_VRF_FALLBACK_GLOBAL_KEY, values))
    {
        m_kernelVrfFallbackEnabled = false;
        return;
    }

    m_kernelVrfFallbackEnabled = parseKernelVrfFallbackState(values);
}

void VrfMgr::handleKernelVrfFallbackConfig(const KeyOpFieldsValuesTuple& t)
{
    const auto& key = kfvKey(t);
    const auto& op = kfvOp(t);

    if (key != KERNEL_VRF_FALLBACK_GLOBAL_KEY)
    {
        SWSS_LOG_ERROR("Ignore invalid key %s in %s; only %s is supported", key.c_str(),
                       KERNEL_VRF_FALLBACK_CONFIG_TABLE, KERNEL_VRF_FALLBACK_GLOBAL_KEY);
        return;
    }

    if (op == SET_COMMAND)
    {
        m_kernelVrfFallbackEnabled = parseKernelVrfFallbackState(kfvFieldsValues(t));
    }
    else if (op == DEL_COMMAND)
    {
        m_kernelVrfFallbackEnabled = false;
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation %s for %s|%s", op.c_str(),
                       KERNEL_VRF_FALLBACK_CONFIG_TABLE, KERNEL_VRF_FALLBACK_GLOBAL_KEY);
        return;
    }

    SWSS_LOG_NOTICE("Kernel VRF fallback is %s",
                    m_kernelVrfFallbackEnabled ? "enabled" : "disabled");
    reconcileAllKernelVrfs();
}

bool VrfMgr::runKernelVrfRouteCommand(uint32_t table, bool ipv6)
{
    stringstream cmd;
    string result;

    cmd << IP_CMD;
    if (ipv6)
    {
        cmd << " -6";
    }

    if (!m_kernelVrfFallbackEnabled)
    {
        cmd << " route replace table " << table << " unreachable default metric "
            << KERNEL_VRF_SENTINEL_METRIC;
        int rc;
        if (!executeKernelVrfCommand(cmd.str(), result, rc))
        {
            return false;
        }
        if (rc != 0)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), rc);
            return false;
        }
        return true;
    }

    cmd << " route del table " << table << " unreachable default metric "
        << KERNEL_VRF_SENTINEL_METRIC;
    int rc;
    if (!executeKernelVrfCommand(cmd.str(), result, rc))
    {
        return false;
    }
    if (rc == 0)
    {
        return true;
    }

    cmd.str("");
    cmd.clear();
    cmd << IP_CMD;
    if (ipv6)
    {
        cmd << " -6";
    }
    cmd << " route show table " << table << " type unreachable";
    if (!executeKernelVrfCommand(cmd.str(), result, rc))
    {
        return false;
    }
    if (rc != 0)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), rc);
        return false;
    }

    if (hasManagedKernelVrfSentinel(result))
    {
        SWSS_LOG_ERROR("Managed unreachable route is still present in table %u", table);
        return false;
    }

    return true;
}

bool VrfMgr::reconcileKernelVrf(const string& vrfName)
{
    auto table = m_vrfTableMap.find(vrfName);
    if (table == m_vrfTableMap.end() || vrfName == MGMT_VRF)
    {
        m_pendingKernelVrfReconcile.erase(vrfName);
        return true;
    }

    bool ipv4Success = runKernelVrfRouteCommand(table->second, false);
    bool ipv6Success = runKernelVrfRouteCommand(table->second, true);
    if (ipv4Success && ipv6Success)
    {
        m_pendingKernelVrfReconcile.erase(vrfName);
        return true;
    }

    m_pendingKernelVrfReconcile.insert(vrfName);
    SWSS_LOG_ERROR("Failed to reconcile kernel VRF fallback routes for %s", vrfName.c_str());
    return false;
}

void VrfMgr::reconcileAllKernelVrfs()
{
    for (auto it = m_pendingKernelVrfReconcile.begin(); it != m_pendingKernelVrfReconcile.end();)
    {
        if (m_vrfTableMap.find(*it) == m_vrfTableMap.end() || *it == MGMT_VRF)
        {
            it = m_pendingKernelVrfReconcile.erase(it);
        }
        else
        {
            ++it;
        }
    }

    vector<string> vrfs;
    vrfs.reserve(m_vrfTableMap.size());
    for (const auto& vrf : m_vrfTableMap)
    {
        vrfs.push_back(vrf.first);
    }

    for (const auto& vrf : vrfs)
    {
        reconcileKernelVrf(vrf);
    }
}

void VrfMgr::retryPendingKernelVrfs()
{
    const vector<string> pending(m_pendingKernelVrfReconcile.begin(),
                                 m_pendingKernelVrfReconcile.end());
    for (const auto& vrf : pending)
    {
        reconcileKernelVrf(vrf);
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
        m_pendingKernelVrfReconcile.erase(vrfName);
        return true;
    }

    cmd << IP_CMD << " link del " << shellquote(vrfName);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    recycleTable(m_vrfTableMap[vrfName]);
    m_vrfTableMap.erase(vrfName);
    m_pendingKernelVrfReconcile.erase(vrfName);

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
    retryPendingKernelVrfs();

    if (consumer.getTableName() == KERNEL_VRF_FALLBACK_CONFIG_TABLE)
    {
        auto config = consumer.m_toSync.begin();
        while (config != consumer.m_toSync.end())
        {
            handleKernelVrfFallbackConfig(config->second);
            config = consumer.m_toSync.erase(config);
        }
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto vrfName = kfvKey(t);

        string op = kfvOp(t);
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
                if (!setLink(vrfName))
                {
                    SWSS_LOG_ERROR("Failed to create vrf netdev %s", vrfName.c_str());
                    it++;
                    continue;
                }

                if (!reconcileKernelVrf(vrfName))
                {
                    SWSS_LOG_ERROR("Kernel VRF fallback reconciliation pending for %s",
                                   vrfName.c_str());
                }

                bool status = true;
                vector<FieldValueTuple> fvVector;
                fvVector.emplace_back("state", "ok");
                m_stateVrfTable.set(vrfName, fvVector);

                SWSS_LOG_NOTICE("Created vrf netdev %s", vrfName.c_str());
                if ((consumer.getTableName() == CFG_VRF_TABLE_NAME) ||
                    (consumer.getTableName() == CFG_MGMT_VRF_CONFIG_TABLE_NAME))
                {
                    status  = doVrfVxlanTableCreateTask (t);
                    if (status == false)
                    {
                        SWSS_LOG_ERROR("VRF VNI Map Config Failed");
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    m_appVrfTableProducer.set(vrfName, kfvFieldsValues(t));

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
                if (!delLink(vrfName))
                {
                    SWSS_LOG_ERROR("Failed to remove vrf netdev %s", vrfName.c_str());
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

    if (m_evpnVxlanTunnel.empty())
    {
        m_evpnVxlanTunnel = tunnel_name;
        VrfVxlanTableSync(true);
    }

    SWSS_LOG_INFO("Added evpn nvo tunnel %s", m_evpnVxlanTunnel.c_str());
    return true;
}

bool VrfMgr::doVrfEvpnNvoDelTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    if (!m_evpnVxlanTunnel.empty())
    {
        VrfVxlanTableSync(false);
        m_evpnVxlanTunnel = "";
    }

    SWSS_LOG_INFO("Removed evpn nvo tunnel %s", m_evpnVxlanTunnel.c_str());
    return true;
}

bool VrfMgr::doVrfVxlanTableCreateTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    auto vrf_name = kfvKey(t);
    uint32_t vni = 0, old_vni = 0;
    string s_vni = "";
    bool add = true;

    const vector<FieldValueTuple>& data = kfvFieldsValues(t);
    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "vni")
        {
            s_vni = value;
            vni = static_cast<uint32_t>(stoul(value));
        }
    }

    if (vni != 0)
    {
        for (auto itr : m_vrfVniMapTable)
        {
            if (vni == itr.second)
            {
                SWSS_LOG_ERROR(" vni %d is already mapped to vrf %s", vni, itr.first.c_str());
                return false;
            }
        }
    }

    old_vni = getVRFmappedVNI(vrf_name);
    SWSS_LOG_INFO("VRF VNI map update vrf %s, vni %d, old_vni %d", vrf_name.c_str(), vni, old_vni);
    if (vni != old_vni)
    {
        if (vni == 0)
        {
            m_vrfVniMapTable.erase(vrf_name);
            s_vni = to_string(old_vni);
            add = false;
        }
        else
        {
            if (old_vni != 0)
            {
                SWSS_LOG_ERROR(" vrf %s is already mapped to vni %d", vrf_name.c_str(), old_vni);
                return false;
            }
            m_vrfVniMapTable[vrf_name] = vni;
        }

    }

    if ((vni == 0) && add)
    {
        return true;
    }

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
        m_vrfVniMapTable.erase(vrf_name);
    }

    return true;
}

bool VrfMgr::doVrfVxlanTableUpdate(const string& vrf_name, const string& vni, bool add)
{
    SWSS_LOG_ENTER();
    string key;

    if (m_evpnVxlanTunnel.empty())
    {
        SWSS_LOG_INFO("NVO Tunnel not present. vrf %s, vni %s, add %d", vrf_name.c_str(), vni.c_str(), add);
        return false;
    }

    key = m_evpnVxlanTunnel + ":" + "evpn_map_" + vni + "_" + vrf_name;

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple v1("vni", vni);
    FieldValueTuple v2("vrf", vrf_name);
    fvVector.push_back(v1);
    fvVector.push_back(v2);

    SWSS_LOG_INFO("VRF VNI map table update vrf %s, vni %s, add %d", vrf_name.c_str(), vni.c_str(), add);
    if (add)
    {
        m_appVxlanVrfTableProducer.set(key, fvVector);
    }
    else
    {
        m_appVxlanVrfTableProducer.del(key);
    }

    return true;
}

void VrfMgr::VrfVxlanTableSync(bool add)
{
    SWSS_LOG_ENTER();
    string s_vni = "";

    for (auto itr : m_vrfVniMapTable)
    {
        s_vni = to_string(itr.second);
        SWSS_LOG_INFO("vrf %s, vni %s, add %d", (itr.first).c_str(), s_vni.c_str(), add);
        doVrfVxlanTableUpdate(itr.first, s_vni, add);
    }
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
