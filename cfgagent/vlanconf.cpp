#include <string.h>
#include "logger.h"
#include "producerstatetable.h"
#include "macaddress.h"
#include "vlanconf.h"
#include "switchconf.h"
#include "exec.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"
#define DEFAULT_VLAN_ID     1

extern MacAddress gMacAddress;
extern SwitchConf *gSwtichConfVlan;

VlanConf::VlanConf(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, vector<string> tableNames) :
        OrchBase(cfgDb, tableNames),
        m_cfgVlanTable(cfgDb, CFG_VLAN_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgVlanMemberTable(cfgDb, CFG_VLAN_MEMBER_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_appVlanTableProducer(appDb, APP_VLAN_TABLE_NAME),
        m_appVlanMemberTableProducer(appDb, APP_VLAN_MEMBER_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    // Initialize Linux dot1q bridge and enable vlan filtering
    string cmd, res;

    cmd = "ip link del ";
    cmd += DOT1Q_BRIDGE_NAME;
    swss::exec(cmd, res);
    cmd = "ip link add ";
    cmd += DOT1Q_BRIDGE_NAME;
    cmd += " up type bridge";
    swss::exec(cmd, res);
    cmd = "echo 1 > /sys/class/net/";
    cmd += DOT1Q_BRIDGE_NAME;
    cmd += "/bridge/vlan_filtering";
    swss::exec(cmd, res);
    cmd = "bridge vlan del vid " + std::to_string(DEFAULT_VLAN_ID)
            + " dev " + DOT1Q_BRIDGE_NAME + " self";
    swss::exec(cmd, res);
}

void VlanConf::syncCfgDB()
{
    OrchBase::syncDB(CFG_VLAN_TABLE_NAME, m_cfgVlanTable);
    OrchBase::syncDB(CFG_VLAN_MEMBER_TABLE_NAME, m_cfgVlanMemberTable);
}

bool VlanConf::addHostVlan(int vlan_id)
{
    string cmd, res;

    cmd = "bridge vlan add vid " + to_string(vlan_id) + " dev "
            + DOT1Q_BRIDGE_NAME + " self";
    swss::exec(cmd, res);
    cmd = "ip link add link ";
    cmd += DOT1Q_BRIDGE_NAME;
    cmd += " name ";
    cmd += VLAN_PREFIX + to_string(vlan_id)
            + " type vlan id " + to_string(vlan_id);
    swss::exec(cmd, res);

    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id);
    cmd += " address " + gMacAddress.to_string();
    swss::exec(cmd, res);

    // Bring up vlan port by default
    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id);
    cmd += " up";
    swss::exec(cmd, res);
    return true;
}

bool VlanConf::removeHostVlan(int vlan_id)
{
    string cmd, res;

    cmd = "ip link del ";
    cmd += VLAN_PREFIX + to_string(vlan_id);
    swss::exec(cmd, res);

    cmd = "bridge vlan del vid " + to_string(vlan_id) + " dev "
            + DOT1Q_BRIDGE_NAME + " self";
    swss::exec(cmd, res);

    return true;
}

bool VlanConf::setHostVlanAdminState(int vlan_id, string &admin_status)
{
    string cmd, res;

    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id) + " " + admin_status;
    swss::exec(cmd, res);
    return true;
}

bool VlanConf::setHostVlanMtu(int vlan_id, uint32_t mtu)
{
    string cmd, res;

    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id) + " mtu " + to_string(mtu);
    swss::exec(cmd, res);
    return true;
}

bool VlanConf::addHostVlanMember(int vlan_id, string &port_alias, string& tagging_mode)
{
    string cmd, res;

    // Should be ok to run set master command more than one time.
    cmd = "ip link set " + port_alias + " master " + DOT1Q_BRIDGE_NAME;
    swss::exec(cmd, res);
    if (tagging_mode == "untagged" || tagging_mode == "priority_tagged")
    {
        // We are setting pvid as untagged vlan id.
        cmd = "bridge vlan add vid " + to_string(vlan_id) + " dev "
            + port_alias + " pvid untagged";
    }
    else
    {
        cmd = "bridge vlan add vid " + to_string(vlan_id) + " dev "
            + port_alias;
    }
    swss::exec(cmd, res);
    // Apply switch level flood control to this port
    gSwtichConfVlan->updateHostFloodControl(port_alias);

    // Bring up vlan member port and set MTU to 9100 by default
    cmd = "ip link set " + port_alias + " up mtu 9100";
    swss::exec(cmd, res);
    return true;
}


bool VlanConf::removeHostVlanMember(int vlan_id, string &port_alias)
{
    string cmd, res;

    cmd = "bridge vlan del vid " + to_string(vlan_id) + " dev "
            + port_alias;
    swss::exec(cmd, res);

    // When port is not member of any VLAN, it shall be detached from Dot1Q bridge!
    cmd = "bridge vlan show dev " + port_alias + " | grep None";
    swss::exec(cmd, res);
    if (!res.empty())
    {
        cmd = "ip link set " + port_alias + " nomaster";
        swss::exec(cmd, res);
    }

    return true;
}

bool VlanConf::isVlanMacOk()
{
    return !(!gMacAddress);
}

void VlanConf::doVlanTask(Consumer &consumer)
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
        vlan_id = stoi(key.substr(4));

        string vlan_alias, port_alias;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            string admin_status;
            uint32_t mtu = 0;
            vector<FieldValueTuple> fvVector;
            string members;

            /* Add host VLAN when it has not been created. */
            if (m_vlans.find(key) == m_vlans.end())
            {
                addHostVlan(vlan_id);
            }

            /* set up host env .... */
            for (auto i : kfvFieldsValues(t))
            {
                /* Set port admin status */
                if (fvField(i) == "admin_status") {
                    admin_status = fvValue(i);
                    setHostVlanAdminState(vlan_id, admin_status);
                    fvVector.push_back(i);
                }
                /* Set port mtu */
                else if (fvField(i) == "mtu") {
                    mtu = (uint32_t)stoul(fvValue(i));
                    setHostVlanMtu(vlan_id, mtu);
                    fvVector.push_back(i);
                }
                else if (fvField(i) == "members@") {
                    members = fvValue(i);
                }
            }
            /* fvVector should not be empty */
            if (fvVector.empty())
            {
                FieldValueTuple a("admin_status",  "up");
                fvVector.push_back(a);
            }
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
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool VlanConf::isMemberStateOk(string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready\n", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("%s is ready\n", alias.c_str());
        return true;
    }
    SWSS_LOG_DEBUG("%s is not ready\n", alias.c_str());
    return false;
}

bool VlanConf::isVlanStateOk(string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("%s is ready\n", alias.c_str());
            return true;
        }
    }
    SWSS_LOG_DEBUG("%s is not ready\n", alias.c_str());
    return false;
}

/*
 * members is grouped in format like
 * "Ethernet1,Ethernet2,Ethernet3,Ethernet4,Ethernet5,Ethernet6,
 * Ethernet7,Ethernet8,Ethernet9,Ethernet10,Ethernet11,Ethernet12,
 * Ethernet13,Ethernet14,Ethernet15,Ethernet16,Ethernet17,Ethernet18,
 * Ethernet19,Ethernet20,Ethernet21,Ethernet22,Ethernet23,Ethernet24"
 */
void VlanConf::processUntaggedVlanMembers(string vlan, string &members)
{

    auto consumer_it = m_consumerMap.find(CFG_VLAN_MEMBER_TABLE_NAME);
    if (consumer_it == m_consumerMap.end())
    {
        SWSS_LOG_ERROR("Failed to find tableName:%s", CFG_VLAN_MEMBER_TABLE_NAME);
        return;
    }
    Consumer& consumer = consumer_it->second;

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
            consumer.m_toSync[member_key] = make_tuple(member_key, SET_COMMAND, fvVector);
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, consumer.m_toSync[member_key])).c_str());
        }
        /*
         * There is pending task from consumber pipe, in this case just skip it.
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

void VlanConf::doVlanMemberTask(Consumer &consumer)
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
                    tagging_mode = fvValue(i);
            }

            if (tagging_mode != "untagged" &&
                tagging_mode != "tagged"   &&
                tagging_mode != "priority_tagged")
            {
                SWSS_LOG_ERROR("Wrong tagging_mode '%s' for key: %s", tagging_mode.c_str(), kfvKey(t).c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            addHostVlanMember(vlan_id, port_alias, tagging_mode);

            key = VLAN_PREFIX + to_string(vlan_id);
            key += DEFAULT_KEY_SEPARATOR;
            key += port_alias;
            m_appVlanMemberTableProducer.set(key, kfvFieldsValues(t));
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            removeHostVlanMember(vlan_id, port_alias);
            key = VLAN_PREFIX + to_string(vlan_id);
            key += DEFAULT_KEY_SEPARATOR;
            key += port_alias;
            m_appVlanMemberTableProducer.del(key);
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void VlanConf::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.m_consumer->getTableName();

    if (table_name == CFG_VLAN_TABLE_NAME)
        doVlanTask(consumer);
    else if (table_name == CFG_VLAN_MEMBER_TABLE_NAME)
        doVlanMemberTask(consumer);
    else
    {
        SWSS_LOG_ERROR("Unknown config table %s ", table_name.c_str());
        throw runtime_error("VlanConf doTask failure.");
    }
}
