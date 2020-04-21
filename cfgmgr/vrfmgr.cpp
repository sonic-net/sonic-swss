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
#define VRF_TABLE_END 2000
#define TABLE_LOCAL_PREF 1001 // after l3mdev-table

using namespace swss;

VrfMgr::VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_appVrfTableProducer(appDb, APP_VRF_TABLE_NAME),
        m_appVnetTableProducer(appDb, APP_VNET_TABLE_NAME),
        m_appVxlanVrfTableProducer(appDb, APP_VXLAN_VRF_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateVrfObjectTable(stateDb, STATE_VRF_OBJECT_TABLE_NAME)
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
                }

                vector<FieldValueTuple> fvVector;
                fvVector.emplace_back("state", "ok");
                m_stateVrfTable.set(vrfName, fvVector);

                SWSS_LOG_NOTICE("Created vrf netdev %s", vrfName.c_str());
                if (consumer.getTableName() == CFG_VRF_TABLE_NAME)
                {
                    m_appVrfTableProducer.set(vrfName, kfvFieldsValues(t));

                    doVrfVxlanTableCreateTask (t);
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
            else if (consumer.getTableName() == CFG_VRF_TABLE_NAME)
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

            if (!delLink(vrfName))
            {
                SWSS_LOG_ERROR("Failed to remove vrf netdev %s", vrfName.c_str());
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

        if (field == "source_vtep") {
            tunnel_name = value;
        }
    }

    if(m_evpnVxlanTunnel.empty()) {
        m_evpnVxlanTunnel = tunnel_name;
        VrfVxlanTableSync(true);
    }

    SWSS_LOG_NOTICE("Added evpn nvo tunnel %s", m_evpnVxlanTunnel.c_str());
    return true;
}

bool VrfMgr::doVrfEvpnNvoDelTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    if(!m_evpnVxlanTunnel.empty()) {
        VrfVxlanTableSync(false);
        m_evpnVxlanTunnel = "";
    }

    SWSS_LOG_NOTICE("Removed evpn nvo tunnel %s", m_evpnVxlanTunnel.c_str());
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

        if (field == "vni") {
            s_vni = value;
            vni = static_cast<uint32_t>(stoul(value));
        }
    }

    old_vni = getVRFmappedVNI(vrf_name);
    SWSS_LOG_NOTICE("VRF VNI map update vrf %s, vni %d, old_vni %d", vrf_name.c_str(), vni, old_vni);
    if (vni != old_vni)
    {
        if (vni == 0) {
            vrf_vni_map_table_.erase(vrf_name);
            s_vni = to_string(old_vni);
            add = false;
        } else {
            vrf_vni_map_table_[vrf_name] = vni;
        }

    }

    if ((vni == 0) && add)
    {
        return true;
    }

    SWSS_LOG_NOTICE("VRF VNI map update vrf %s, s_vni %s, add %d", vrf_name.c_str(), s_vni.c_str(), add);
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
    SWSS_LOG_NOTICE("VRF VNI map remove vrf %s, vni %d", vrf_name.c_str(), vni);
    if (vni != 0) {
        s_vni = to_string(vni);
        doVrfVxlanTableUpdate(vrf_name, s_vni, false);
        vrf_vni_map_table_.erase(vrf_name);
    }

    return true;
}

bool VrfMgr::doVrfVxlanTableUpdate(const string& vrf_name, const string& vni, bool add)
{
    SWSS_LOG_ENTER();
    string key;

    if(m_evpnVxlanTunnel.empty()) {
        SWSS_LOG_NOTICE("NVO Tunnel not present. vrf %s, vni %s, add %d", vrf_name.c_str(), vni.c_str(), add);
        return false;
    }

    key = m_evpnVxlanTunnel + ":" + "evpn_map_" + vni + "_" + vrf_name;

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple v1("vni", vni);
    FieldValueTuple v2("vrf", vrf_name);
    fvVector.push_back(v1);
    fvVector.push_back(v2);

    SWSS_LOG_NOTICE("VRF VNI map table update vrf %s, vni %s, add %d", vrf_name.c_str(), vni.c_str(), add);
    if (add) {
        m_appVxlanVrfTableProducer.set(key, fvVector);
    } else {
        m_appVxlanVrfTableProducer.del(key);
    }

    return true;
}

void VrfMgr::VrfVxlanTableSync(bool add)
{
    SWSS_LOG_ENTER();
    string s_vni = "";

    for (auto itr : vrf_vni_map_table_)
    {
        s_vni = to_string(itr.second);
        SWSS_LOG_NOTICE("vrf %s, vni %s, add %d", (itr.first).c_str(), s_vni.c_str(), add);
        doVrfVxlanTableUpdate(itr.first, s_vni, add);
    }
}

