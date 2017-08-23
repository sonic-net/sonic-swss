#include <string.h>
#include <errno.h>
#include <system_error>
#include <iostream>
#include <set>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "cfgagent/vlancfgagent.h"
#include "cfgagent/switchcfgagent.h"
#include "exec.h"


using namespace std;
using namespace swss;

#define DOT1Q_BRIDGE_NAME   "Bridge"
#define VLAN_PREFIX         "Vlan"
#define DEFAULT_VLAN_ID     1

extern bool gInitDone;
extern MacAddress gMacAddress;
extern SwitchCfgAgent *gSwtichcfgagent;

VlanCfgAgent::VlanCfgAgent(DBConnector *cfgDb, DBConnector *appDb, vector<string> tableNames) :
        CfgOrch(cfgDb, tableNames),
        m_cfgVlanTableConsumer(cfgDb, CFG_VLAN_TABLE_NAME),
        m_cfgVlanMemberTableConsumer(cfgDb, CFG_VLAN_MEMBER_TABLE_NAME),
        m_vlanTableProducer(appDb, APP_VLAN_TABLE_NAME),
        m_vlanMemberTableProducer(appDb, APP_VLAN_MEMBER_TABLE_NAME)

{
    SWSS_LOG_ENTER();

    // Initialize Linux dot1q bridge and enable vlan filtering
    string cmd;

    cmd = "ip link del ";
    cmd += DOT1Q_BRIDGE_NAME;
    swss::exec(cmd.c_str());
    cmd = "ip link add ";
    cmd += DOT1Q_BRIDGE_NAME;
    cmd += " up type bridge";
    swss::exec(cmd.c_str());
    cmd = "echo 1 > /sys/class/net/";
    cmd += DOT1Q_BRIDGE_NAME;
    cmd += "/bridge/vlan_filtering";
    swss::exec(cmd.c_str());
    cmd = "bridge vlan del vid " + std::to_string(DEFAULT_VLAN_ID)
            + " dev " + DOT1Q_BRIDGE_NAME + " self";
    swss::exec(cmd.c_str());
}

void VlanCfgAgent::syncCfgDB()
{
    CfgOrch::syncCfgDB(CFG_VLAN_TABLE_NAME, m_cfgVlanTableConsumer);
    CfgOrch::syncCfgDB(CFG_VLAN_MEMBER_TABLE_NAME, m_cfgVlanMemberTableConsumer);
}

bool VlanCfgAgent::addHostVlan(int vlan_id)
{
    string cmd;

    cmd = "bridge vlan add vid " + to_string(vlan_id) + " dev "
            + DOT1Q_BRIDGE_NAME + " self";
    swss::exec(cmd.c_str());
    cmd = "ip link add link ";
    cmd += DOT1Q_BRIDGE_NAME;
    cmd += " name ";
    cmd += VLAN_PREFIX + to_string(vlan_id)
            + " type vlan id " + to_string(vlan_id);
    swss::exec(cmd.c_str());

    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id);
    cmd += " address " + gMacAddress.to_string();
    swss::exec(cmd.c_str());
    return true;
}

bool VlanCfgAgent::removeHostVlan(int vlan_id)
{
    string cmd;

    cmd = "ip link del ";
    cmd += VLAN_PREFIX + to_string(vlan_id);
    swss::exec(cmd.c_str());

    cmd = "bridge vlan del vid " + to_string(vlan_id) + " dev "
            + DOT1Q_BRIDGE_NAME + " self";
    swss::exec(cmd.c_str());

    return true;
}

bool VlanCfgAgent::setHostVlanAdminState(int vlan_id, string &admin_status)
{
    string cmd;

    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id) + " " + admin_status;
    swss::exec(cmd.c_str());
    return true;
}

bool VlanCfgAgent::setHostVlanMtu(int vlan_id, uint32_t mtu)
{
    string cmd;

    cmd = "ip link set ";
    cmd += VLAN_PREFIX + to_string(vlan_id) + " mtu " + to_string(mtu);
    swss::exec(cmd.c_str());
    return true;
}

bool VlanCfgAgent::addHostVlanMember(int vlan_id, string &port_alias, string& tagging_mode)
{
    string cmd;

    // Should be ok to run set master command more than one time.
    cmd = "ip link set " + port_alias + " master " + DOT1Q_BRIDGE_NAME;
    swss::exec(cmd.c_str());
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
    swss::exec(cmd.c_str());
    // Apply switch level flood control to this port
    gSwtichcfgagent->updateHostFloodControl(port_alias);
    return true;
}


bool VlanCfgAgent::removeHostVlanMember(int vlan_id, string &port_alias)
{
    string cmd, res;

    cmd = "bridge vlan del vid " + to_string(vlan_id) + " dev "
            + port_alias;
    swss::exec(cmd.c_str());

    // When port is not member of any VLAN, it shall be detached from Dot1Q bridge!
    cmd = "bridge vlan show dev " + port_alias + " | grep None";
    res = swss::exec(cmd.c_str());
    if (! res.empty())
    {
        cmd = "ip link set " + port_alias + " nomaster";
        swss::exec(cmd.c_str());
    }

    return true;
}

void VlanCfgAgent::doVlanTask(Consumer &consumer)
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

        int vlan_id;
        vlan_id = stoi(key.substr(4)); // FIXME: might raise exception

        string vlan_alias, port_alias;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<FieldValueTuple> fvVector;
            string admin_status;
            uint32_t mtu = 0;

            // Add host VLAN when it has not been created.
            if (m_vlans.find(key) == m_vlans.end())
            {
                addHostVlan(vlan_id);
            }

            // set up host env ....
            for (auto i : kfvFieldsValues(t))
            {
                /* Set port admin status */
                if (fvField(i) == "admin_status") {
                    admin_status = fvValue(i);
                    setHostVlanAdminState(vlan_id, admin_status);
                }
                /* Set port mtu */
                else if (fvField(i) == "mtu") {
                    mtu = (uint32_t)stoul(fvValue(i));
                    setHostVlanMtu(vlan_id, mtu);
                }
                /*
                 * fields: unicast_miss_flood, multicast_miss_flood,
                 * broadcast_flood,  and autostate are for lower
                 * layer procesing. VLAN scope flood control mechanism
                 * is not available yet.
                 *
                 * fileds: description, for upper layer only?
                 *
                 * fileds: config_status_code, not implmentated yet.
                 */
                /* Set port autostate */
                // string autostate = false;
                // if (fvField(i) == "autostate")
                //    autostate = stoul(fvValue(i));
            }

            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            m_vlanTableProducer.set(key, kfvFieldsValues(t));
            m_vlans.insert(key);

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_vlans.find(key) != m_vlans.end())
            {
                removeHostVlan(vlan_id);
                m_vlans.erase(key);
                m_vlanTableProducer.del(key);
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

void VlanCfgAgent::doVlanMemberTask(Consumer &consumer)
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
        size_t found = key.find(':');
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
            m_vlanMemberTableProducer.set(kfvKey(t), kfvFieldsValues(t));
            SWSS_LOG_DEBUG("%s", (dumpTuple(consumer, t)).c_str());
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            removeHostVlanMember(vlan_id, port_alias);
            m_vlanMemberTableProducer.del(kfvKey(t));
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

void VlanCfgAgent::doTask(Consumer &consumer)
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
        throw runtime_error("VlanCfgAgent doTask failure.");
    }
}
