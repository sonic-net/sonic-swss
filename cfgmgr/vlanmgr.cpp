#include <string.h>
#include "logger.h"
#include "producerstatetable.h"
#include "macaddress.h"
#include "vlanmgr.h"
#include "exec.h"
#include "tokenize.h"
#include "shellcmd.h"
#include "warm_restart.h"
#include <swss/redisutility.h>

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"
#define DEFAULT_VLAN_ID     "1"
#define DEFAULT_MTU_STR     "9100"
#define VLAN_HLEN            4

extern MacAddress gMacAddress;

VlanMgr::VlanMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgVlanTable(cfgDb, CFG_VLAN_TABLE_NAME),
        m_cfgVlanMemberTable(cfgDb, CFG_VLAN_MEMBER_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateVlanMemberTable(stateDb, STATE_VLAN_MEMBER_TABLE_NAME),
        m_appVlanTableProducer(appDb, APP_VLAN_TABLE_NAME),
        m_appVlanMemberTableProducer(appDb, APP_VLAN_MEMBER_TABLE_NAME),
        replayDone(false)
{
    SWSS_LOG_ENTER();

    if (WarmStart::isWarmStart())
    {
        vector<string> vlanKeys, vlanMemberKeys;

        /* cache all vlan and vlan member config */
        m_cfgVlanTable.getKeys(vlanKeys);
        m_cfgVlanMemberTable.getKeys(vlanMemberKeys);
        for (auto k : vlanKeys)
        {
            m_vlanReplay.insert(k);
        }
        for (auto k : vlanMemberKeys)
        {
            m_vlanMemberReplay.insert(k);
        }
        if (m_vlanReplay.empty())
        {
            replayDone = true;
            WarmStart::setWarmStartState("vlanmgrd", WarmStart::REPLAYED);
            SWSS_LOG_NOTICE("vlanmgr warmstart state set to REPLAYED");
            WarmStart::setWarmStartState("vlanmgrd", WarmStart::RECONCILED);
            SWSS_LOG_NOTICE("vlanmgr warmstart state set to RECONCILED");
        }
        const std::string cmds = std::string("")
          + IP_CMD + " link show " + DOT1Q_BRIDGE_NAME + " 2>/dev/null";

        std::string res;
        int ret = swss::exec(cmds, res);
        if (ret == 0)
        {
            // Don't reset vlan aware bridge upon swss docker warm restart.
            SWSS_LOG_INFO("vlanmgrd warm start, skipping bridge create");
            return;
        }
    }
    // Initialize Linux dot1q bridge and enable vlan filtering
    // The command should be generated as:
    // /bin/bash -c "/sbin/ip link del Bridge 2>/dev/null ;
    //               /sbin/ip link add Bridge up type bridge &&
    //               /sbin/ip link set Bridge mtu {{ mtu_size }} &&
    //               /sbin/ip link set Bridge address {{gMacAddress}} &&
    //               /sbin/bridge vlan del vid 1 dev Bridge self;
    //               /sbin/ip link del dummy 2>/dev/null;
    //               /sbin/ip link add dummy type dummy &&
    //               /sbin/ip link set dummy master Bridge"

    const std::string cmds = std::string("")
      + BASH_CMD + " -c \""
      + IP_CMD + " link del " + DOT1Q_BRIDGE_NAME + " 2>/dev/null; "
      + IP_CMD + " link add " + DOT1Q_BRIDGE_NAME + " up type bridge && "
      + IP_CMD + " link set " + DOT1Q_BRIDGE_NAME + " mtu " + DEFAULT_MTU_STR + " && "
      + IP_CMD + " link set " + DOT1Q_BRIDGE_NAME + " address " + gMacAddress.to_string() + " && "
      + BRIDGE_CMD + " vlan del vid " + DEFAULT_VLAN_ID + " dev " + DOT1Q_BRIDGE_NAME + " self; "
      + IP_CMD + " link del dev dummy 2>/dev/null; "
      + IP_CMD + " link add dummy type dummy && "
      + IP_CMD + " link set dummy master " + DOT1Q_BRIDGE_NAME + "\"";

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds, res);

    // /sbin/ip link set Bridge type bridge vlan_filtering 1
    const std::string vlan_filtering_cmd = std::string(IP_CMD) + " link set " + DOT1Q_BRIDGE_NAME + " type bridge vlan_filtering 1";
    EXEC_WITH_ERROR_THROW(vlan_filtering_cmd, res);

    // /sbin/ip link set Bridge type bridge no_linklocal_learn 1
    const std::string no_ll_learn_cmd = std::string(IP_CMD) + " link set " + DOT1Q_BRIDGE_NAME + " type bridge no_linklocal_learn 1";
    EXEC_WITH_ERROR_THROW(no_ll_learn_cmd, res);
}

bool VlanMgr::addHostVlan(int vlan_id)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c "/sbin/bridge vlan add vid {{vlan_id}} dev Bridge self &&
    //               /sbin/ip link add link Bridge up name Vlan{{vlan_id}} address {{gMacAddress}} type vlan id {{vlan_id}}"
    const std::string cmds = std::string("")
      + BASH_CMD + " -c \""
      + BRIDGE_CMD + " vlan add vid " + std::to_string(vlan_id) + " dev " + DOT1Q_BRIDGE_NAME + " self && "
      + IP_CMD + " link add link " + DOT1Q_BRIDGE_NAME
               + " up"
               + " name " + VLAN_PREFIX + std::to_string(vlan_id)
               + " address " + gMacAddress.to_string()
               + " type vlan id " + std::to_string(vlan_id) + "\"";

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds, res);

    res.clear();
    const std::string echo_cmd = std::string("")
      + ECHO_CMD + " 0 > /proc/sys/net/ipv4/conf/" + VLAN_PREFIX + std::to_string(vlan_id) + "/arp_evict_nocarrier";
    swss::exec(echo_cmd, res);

    return true;
}

bool VlanMgr::removeHostVlan(int vlan_id)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c "/sbin/ip link del Vlan{{vlan_id}} &&
    //               /sbin/bridge vlan del vid {{vlan_id}} dev Bridge self"
    const std::string cmds = std::string("")
      + BASH_CMD + " -c \""
      + IP_CMD + " link del " + VLAN_PREFIX + std::to_string(vlan_id) + " && "
      + BRIDGE_CMD + " vlan del vid " + std::to_string(vlan_id) + " dev " + DOT1Q_BRIDGE_NAME + " self\"";

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds, res);

    return true;
}

bool VlanMgr::setHostVlanAdminState(int vlan_id, const string &admin_status)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /sbin/ip link set Vlan{{vlan_id}} {{admin_status}}
    ostringstream cmds;
    cmds << IP_CMD " link set " VLAN_PREFIX + std::to_string(vlan_id) + " " << shellquote(admin_status);

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds.str(), res);

    return true;
}

bool VlanMgr::setHostVlanMtu(int vlan_id, uint32_t mtu)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /sbin/ip link set Vlan{{vlan_id}} mtu {{mtu}}
    const std::string cmds = std::string("")
      + IP_CMD + " link set " + VLAN_PREFIX + std::to_string(vlan_id) + " mtu " + std::to_string(mtu);

    std::string res;
    int ret = swss::exec(cmds, res);
    if (ret == 0)
    {
        return true;
    }

    /* VLAN mtu should not be larger than member mtu */
    return false;
}

bool VlanMgr::setHostVlanMac(int vlan_id, const string &mac)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /sbin/ip link set Vlan{{vlan_id}} address {{mac}}
    ostringstream cmds;
    cmds << IP_CMD " link set " VLAN_PREFIX + std::to_string(vlan_id) + " address " << shellquote(mac) << " && "
            IP_CMD " link set " DOT1Q_BRIDGE_NAME " address " << shellquote(mac);

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds.str(), res);

    return true;
}

bool VlanMgr::addHostVlanMember(int vlan_id, const string &port_alias, const string& tagging_mode)
{
    SWSS_LOG_ENTER();

    std::string tagging_cmd;
    if (tagging_mode == "untagged" || tagging_mode == "priority_tagged")
    {
        tagging_cmd = "pvid untagged";
    }

    // The command should be generated as:
    // /bin/bash -c "/sbin/ip link set {{port_alias}} master Bridge &&
    //               /sbin/bridge vlan del vid 1 dev {{ port_alias }} &&
    //               /sbin/bridge vlan add vid {{vlan_id}} dev {{port_alias}} {{tagging_mode}}"
    ostringstream cmds, inner;
    inner << IP_CMD " link set " << shellquote(port_alias) << " master " DOT1Q_BRIDGE_NAME " && "
      BRIDGE_CMD " vlan del vid " DEFAULT_VLAN_ID " dev " << shellquote(port_alias) << " && "
      BRIDGE_CMD " vlan add vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " " + tagging_cmd;
    cmds << BASH_CMD " -c " << shellquote(inner.str());

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds.str(), res);

    return true;
}

bool VlanMgr::removeHostVlanMember(int vlan_id, const string &port_alias)
{
    SWSS_LOG_ENTER();

    // The command should be generated as:
    // /bin/bash -c '/sbin/bridge vlan del vid {{vlan_id}} dev {{port_alias}} &&
    //               ( vlanShow=$(/sbin/bridge vlan show dev {{port_alias}});
    //               ret=$?;
    //               if [ $ret -eq 0 ]; then
    //               if (! echo "$vlanShow" | grep -q {{port_alias}})
    //                 || (echo "$vlanShow" | grep -q None$)
    //                 || (echo "$vlanShow" | grep -q {{port_alias}}$); then
    //               /sbin/ip link set {{port_alias}} nomaster;
    //               fi;
    //               else exit $ret; fi )'

    // When port is not member of any VLAN, it shall be detached from Dot1Q bridge!
    ostringstream cmds, inner;
    inner << BRIDGE_CMD " vlan del vid " + std::to_string(vlan_id) + " dev " << shellquote(port_alias) << " && ( "
      "vlanShow=$(" BRIDGE_CMD " vlan show dev " << shellquote(port_alias) << "); "
      "ret=$?; "
      "if [ $ret -eq 0 ]; then "
      "if (! echo \"$vlanShow\" | " GREP_CMD " -q " << shellquote(port_alias) << ") "
      " || (echo \"$vlanShow\" | " GREP_CMD " -q None$) "
      " || (echo \"$vlanShow\" | " GREP_CMD " -q " << shellquote(port_alias) << "$); then "
      IP_CMD " link set " << shellquote(port_alias) << " nomaster; "
      "fi; "
      "else exit $ret; fi )";
    cmds << BASH_CMD " -c " << shellquote(inner.str());

    std::string res;
    EXEC_WITH_ERROR_THROW(cmds.str(), res);

    return true;
}

bool VlanMgr::isVlanMacOk()
{
    return !!gMacAddress;
}

void VlanMgr::doVlanTask(Consumer &consumer)
{
    if (!isVlanMacOk())
    {
        SWSS_LOG_DEBUG("VLAN mac not ready, delaying VLAN task");
        return;
    }
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        int vlan_id;
        try
        {
            vlan_id = stoi(key.substr(4));
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Invalid key format. Not a number after 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string vlan_alias, port_alias;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string admin_status;
            string mtu = DEFAULT_MTU_STR;
            string mac = gMacAddress.to_string();
            string hostif_name = "";
            vector<FieldValueTuple> fvVector;
            string members;

            /*
             * If state is already set for this vlan, but it doesn't exist in m_vlans set,
             * just add it to m_vlans set and remove the request to skip disrupting Linux vlan.
             * Will hit this scenario for docker warm restart.
             *
             * Otherwise, it is new VLAN create or VLAN attribute update like admin_status/mtu change,
             * proceed with regular processing.
             */
            if (isVlanStateOk(key) && m_vlans.find(key) == m_vlans.end())
            {
                SWSS_LOG_DEBUG("%s already created", kfvKey(t).c_str());
                m_vlans.insert(key);
                m_vlanReplay.erase(kfvKey(t));
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Add host VLAN when it has not been created. */
            if (m_vlans.find(key) == m_vlans.end())
            {
                addHostVlan(vlan_id);
            }
            m_vlanReplay.erase(kfvKey(t));

            /* set up host env .... */
            for (auto i : kfvFieldsValues(t))
            {
                /* Set vlan admin status */
                if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);
                    setHostVlanAdminState(vlan_id, admin_status);
                    fvVector.push_back(i);
                }
                /* Set vlan mtu */
                else if (fvField(i) == "mtu")
                {
                    mtu = fvValue(i);
                    /*
                     * TODO: support host VLAN mtu setting.
                     * Host VLAN mtu should be set only after member configured
                     * and VLAN state is not UNKNOWN.
                     */
                    SWSS_LOG_DEBUG("%s mtu %s: Host VLAN mtu setting to be supported.", key.c_str(), mtu.c_str());
                }
                else if (fvField(i) == "members@") {
                    members = fvValue(i);
                }
                else if (fvField(i) == "mac")
                {
                    mac = fvValue(i);
                    setHostVlanMac(vlan_id, mac);
                }
                else if (fvField(i) == "host_ifname")
                {
                    hostif_name = fvValue(i);
                }
            }
            /* fvVector should not be empty */
            if (fvVector.empty())
            {
                FieldValueTuple a("admin_status",  "up");
                fvVector.push_back(a);
            }

            FieldValueTuple m("mtu", mtu);
            fvVector.push_back(m);

            FieldValueTuple mc("mac", mac);
            fvVector.push_back(mc);

            FieldValueTuple hostif_name_fvt("host_ifname", hostif_name);
            fvVector.push_back(hostif_name_fvt);

            m_appVlanTableProducer.set(key, fvVector);
            m_vlans.insert(key);

            fvVector.clear();
            FieldValueTuple s("state", "ok");
            fvVector.push_back(s);
            m_stateVlanTable.set(key, fvVector);

            it = consumer.m_toSync.erase(it);

            /*
             * Members configured together with VLAN in untagged mode.
             * This is to be compatible with access VLAN configuration from minigraph.
             */
            if (!members.empty())
            {
                processUntaggedVlanMembers(key, members);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_vlans.find(key) != m_vlans.end())
            {
                removeHostVlan(vlan_id);
                m_vlans.erase(key);
                m_appVlanTableProducer.del(key);
                m_stateVlanTable.del(key);
            }
            else
            {
                SWSS_LOG_ERROR("%s doesn't exist", key.c_str());
            }
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
    if (!replayDone && m_vlanReplay.empty() &&
        m_vlanMemberReplay.empty() &&
        WarmStart::isWarmStart())
    {
        replayDone = true;
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::REPLAYED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to REPLAYED");
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::RECONCILED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to RECONCILED");
    }
}

bool VlanMgr::isMemberStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        auto state_opt = swss::fvsGetValue(temp, "state", true);
        if (!state_opt)
        {
            return false;
        }
        SWSS_LOG_DEBUG("%s is ready", alias.c_str());
        return true;
    }
    SWSS_LOG_DEBUG("%s is not ready", alias.c_str());
    return false;
}

bool VlanMgr::isVlanStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready", alias.c_str());
            return true;
        }
    }
    SWSS_LOG_DEBUG("%s is not ready", alias.c_str());
    return false;
}

bool VlanMgr::isVlanMemberStateOk(const string &vlanMemberKey)
{
    vector<FieldValueTuple> temp;

    if (m_stateVlanMemberTable.get(vlanMemberKey, temp))
    {
        SWSS_LOG_DEBUG("%s is ready", vlanMemberKey.c_str());
        return true;
    }
    return false;
}

/*
 * members is grouped in format like
 * "Ethernet1,Ethernet2,Ethernet3,Ethernet4,Ethernet5,Ethernet6,
 * Ethernet7,Ethernet8,Ethernet9,Ethernet10,Ethernet11,Ethernet12,
 * Ethernet13,Ethernet14,Ethernet15,Ethernet16,Ethernet17,Ethernet18,
 * Ethernet19,Ethernet20,Ethernet21,Ethernet22,Ethernet23,Ethernet24"
 */
void VlanMgr::processUntaggedVlanMembers(string vlan, const string &members)
{

    auto consumer_it = m_consumerMap.find(CFG_VLAN_MEMBER_TABLE_NAME);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Failed to find tableName:%s", CFG_VLAN_MEMBER_TABLE_NAME);
        return;
    }
    auto& consumer = static_cast<Consumer &>(*consumer_it->second);

    vector<string> vlanMembers = tokenize(members, ',');

    for (auto vlanMember : vlanMembers)
    {
        string member_key = vlan + CONFIGDB_KEY_SEPARATOR + vlanMember;

        /* Directly put it into consumer.m_toSync map */
        if (consumer.m_toSync.find(member_key) == consumer.m_toSync.end())
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple t("tagging_mode", "untagged");
            fvVector.push_back(t);
            KeyOpFieldsValuesTuple tuple = make_tuple(member_key, SET_COMMAND, fvVector);
            consumer.addToSync(tuple);
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(tuple)).c_str());
        }
        /*
         * There is pending task from consumer pipe, in this case just skip it.
         */
        else
        {
            SWSS_LOG_WARN("Duplicate key %s found in table:%s", member_key.c_str(), CFG_VLAN_MEMBER_TABLE_NAME);
            continue;
        }
    }

    doTask(consumer);
    return;
}

void VlanMgr::doVlanMemberTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        key = key.substr(4);
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);
        int vlan_id;
        string vlan_alias, port_alias;
        if (found != string::npos)
        {
            vlan_id = stoi(key.substr(0, found));
            port_alias = key.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format. No member port is presented: %s",
                           kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        string op = kfvOp(t);

       // TODO:  store port/lag/VLAN data in local data structure and perform more validations.
        if (op == SET_COMMAND)
        {
             if (isVlanMemberStateOk(kfvKey(t)))
             {
                SWSS_LOG_DEBUG("%s already set", kfvKey(t).c_str());
                m_vlanMemberReplay.erase(kfvKey(t));
                it = consumer.m_toSync.erase(it);
                continue;
             }

            /* Don't proceed if member port/lag is not ready yet */
            if (!isMemberStateOk(port_alias) || !isVlanStateOk(vlan_alias))
            {
                SWSS_LOG_DEBUG("%s not ready, delaying", kfvKey(t).c_str());
                it++;
                continue;
            }
            string tagging_mode = "untagged";

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "tagging_mode")
                {
                    tagging_mode = fvValue(i);
                }
            }

            if (tagging_mode != "untagged" &&
                tagging_mode != "tagged"   &&
                tagging_mode != "priority_tagged")
            {
                SWSS_LOG_ERROR("Wrong tagging_mode '%s' for key: %s", tagging_mode.c_str(), kfvKey(t).c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addHostVlanMember(vlan_id, port_alias, tagging_mode))
            {
                key = VLAN_PREFIX + to_string(vlan_id);
                key += DEFAULT_KEY_SEPARATOR;
                key += port_alias;
                m_appVlanMemberTableProducer.set(key, kfvFieldsValues(t));

                vector<FieldValueTuple> fvVector;
                FieldValueTuple s("state", "ok");
                fvVector.push_back(s);
                m_stateVlanMemberTable.set(kfvKey(t), fvVector);

                m_vlanMemberReplay.erase(kfvKey(t));
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (isVlanMemberStateOk(kfvKey(t)))
            {
                removeHostVlanMember(vlan_id, port_alias);
                key = VLAN_PREFIX + to_string(vlan_id);
                key += DEFAULT_KEY_SEPARATOR;
                key += port_alias;
                m_appVlanMemberTableProducer.del(key);
                m_stateVlanMemberTable.del(kfvKey(t));
            }
            else
            {
                SWSS_LOG_DEBUG("%s doesn't exist", kfvKey(t).c_str());
            }
            SWSS_LOG_DEBUG("%s", (consumer.dumpTuple(t)).c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        /* Other than the case of member port/lag is not ready, no retry will be performed */
        it = consumer.m_toSync.erase(it);
    }
    if (!replayDone && m_vlanMemberReplay.empty() &&
        WarmStart::isWarmStart())
    {
        replayDone = true;
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::REPLAYED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to REPLAYED");
        WarmStart::setWarmStartState("vlanmgrd", WarmStart::RECONCILED);
        SWSS_LOG_NOTICE("vlanmgr warmstart state set to RECONCILED");

    }
}

void VlanMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == CFG_VLAN_TABLE_NAME)
    {
        doVlanTask(consumer);
    }
    else if (table_name == CFG_VLAN_MEMBER_TABLE_NAME)
    {
        doVlanMemberTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
        throw runtime_error("VlanMgr doTask failure.");
    }
}
