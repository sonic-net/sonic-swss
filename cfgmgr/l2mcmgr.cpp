#include "exec.h"
#include "l2mcmgr.h"
#include "tokenize.h"
#include "warm_restart.h"
#include "logger.h"

#include <string>
#include <sstream>
#include <iostream>
#include <stdlib.h>

using namespace std;
using namespace swss;

#define MLD_IP_IPV4_AFI 1
#define MLD_IP_IPV6_AFI 2

#define TAGGED 1
#define UNTAGGED 0

#define L2MCD_MAX_SIZE 16318

static char buffer[L2MCD_MAX_SIZE] = {0};


L2McMgr::L2McMgr(DBConnector *confDb, DBConnector *applDb, DBConnector *statDb,
        const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgL2McGlobalTable(confDb, CFG_L2MC_TABLE_NAME),
    m_cfgL2McMldGlobalTable(confDb, CFG_MLD_L2MC_TABLE_NAME),
    m_cfgL2McStaticTable(confDb, CFG_L2MC_STATIC_TABLE_NAME),
    m_cfgL2McMldStaticTable(confDb, CFG_MLD_L2MC_STATIC_TABLE_NAME),
    m_cfgL2McMrouterTable(confDb, CFG_L2MC_MROUTER_TABLE_NAME),
    m_cfgL2McMldMrouterTable(confDb, CFG_MLD_L2MC_MROUTER_TABLE_NAME),
    m_cfgVlanMemberTable(confDb, CFG_VLAN_MEMBER_TABLE_NAME),
    m_stateVlanTable(statDb, STATE_VLAN_TABLE_NAME),
    m_stateVlanMemberTable(statDb, STATE_VLAN_MEMBER_TABLE_NAME),
    m_stateInterfaceTableName(statDb,STATE_INTERFACE_TABLE_NAME),
    m_statel2mcdLocalMemberTable(statDb, STATE_L2MC_MEMBER_TABLE_NAME),
    m_statel2mcdLocalMrouterTable(statDb,STATE_L2MC_MROUTER_TABLE_NAME),
    m_cfgLagMemberTable(confDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_statePortTable(statDb, STATE_PORT_TABLE_NAME),
    m_stateLagTable(statDb, STATE_LAG_TABLE_NAME),
    m_appPortTable(applDb, APP_PORT_TABLE_NAME),
    m_appLagTable(applDb,APP_LAG_TABLE_NAME),
    m_appL2mcVlanTable(applDb,APP_L2MC_VLAN_TABLE_NAME),
    m_appL2mcGrpMemTable(applDb,APP_L2MC_MEMBER_TABLE_NAME),
    m_appL2mcMrouterTable(applDb,APP_L2MC_MROUTER_TABLE_NAME),
    m_appL2mcSuppressTableProducer(applDb,APP_L2MC_SUPPRESS_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Add REDIS DB L2mc entry notification support");
    m_l2mcNotificationConsumer = new swss::NotificationConsumer(statDb, "L2MC_NOTIFICATIONS_REMOTE");
    auto l2mcNotificatier = new Notifier(m_l2mcNotificationConsumer, this, "L2MC_NOTIFICATIONS_REMOTE");
    Orch::addExecutor(l2mcNotificatier);

    SWSS_LOG_INFO("Add REDIS DB mrouter notification support");
    m_l2mcMrouterNotificationConsumer = new swss::NotificationConsumer(statDb, "L2MC_MROUTER_NOTIFICATIONS_REMOTE");
    auto l2mcMrouterNotificatier = new Notifier(m_l2mcMrouterNotificationConsumer, this, "L2MC_MROUTER_NOTIFICATIONS_REMOTE");
    Orch::addExecutor(l2mcMrouterNotificatier);

    m_l2mcCfgparaDoneNotificationConsumer = new swss::NotificationConsumer(applDb, "L2MC_CONFIG_PARA_DONE");
    auto l2mcCfgparaDoneNotificatier = new Notifier(m_l2mcCfgparaDoneNotificationConsumer, this, "L2MC_CONFIG_PARA_DONE");
    Orch::addExecutor(l2mcCfgparaDoneNotificatier);

}

void L2McMgr::doTask(Consumer &consumer)
{
    auto table = consumer.getTableName();
    if (table == CFG_L2MC_TABLE_NAME)
    {
        doL2McGlobalTask(consumer);
    }
    else if (table == CFG_L2MC_STATIC_TABLE_NAME)
    {
        doL2McStaticEntryTask(consumer);
    }
    else if (table == STATE_VLAN_TABLE_NAME)
    {
        doVlanUpdateTask(consumer);
    }
    else if (table == STATE_VLAN_MEMBER_TABLE_NAME)
    {
        doL2McVlanMemUpdateTask(consumer);
    }
    else if (table == STATE_INTERFACE_TABLE_NAME)
    {
        doL2McL3InterfaceUpdateTask(consumer);
    }
    else if (table == CFG_L2MC_MROUTER_TABLE_NAME)
    {
        doL2McMrouterUpdateTask(consumer);
    }
    else if (table == CFG_LAG_MEMBER_TABLE_NAME)
    {
        doL2McLagMemberUpdateTask(consumer);
    }
    else if ((table == STATE_LAG_TABLE_NAME)|| (table == STATE_PORT_TABLE_NAME))
    {
        doL2McInterfaceUpdateTask(consumer);
    }
    else if (table == CFG_L2MC_SUPPRESS_TABLE_NAME)
    {
        SWSS_LOG_NOTICE("enter table %s", table.c_str());
        doL2McSuppressUpdateTask(consumer);
    }
    else if (table == CFG_MLD_L2MC_TABLE_NAME)
    {
        SWSS_LOG_NOTICE("enter table %s", table.c_str());
        doL2McMldGlobalTask(consumer);
    }
    else if (table == CFG_MLD_L2MC_STATIC_TABLE_NAME)
    {
        doL2McMldStaticEntryTask(consumer);
    }
    else if (table == CFG_MLD_L2MC_MROUTER_TABLE_NAME)
    {
        doL2McMldMrouterUpdateTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table.c_str());
    }
}

void L2McMgr::doL2McGlobalTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op  = kfvOp(t);
        string vlanKey = key.substr(4); // Remove "Vlan"
        int vlan_id = stoi(vlanKey);

        bool enabled = false;
        bool exist_enabled = false;

        auto vlan_it = m_vlanIgmpSnoopMap.find(vlan_id);
        if (vlan_it != m_vlanIgmpSnoopMap.end())
            exist_enabled = true;

        vector<PORT_ATTR> port_list;
        uint32_t port_count = getVlanMembers(key, port_list);

        msg_len = sizeof(L2MCD_CONFIG_MSG);
        msg_len += (uint32_t)(port_count * sizeof(PORT_ATTR));
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlan_id);

            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->cmd_code  = 0;
        msg->query_interval = 125;
        msg->query_max_response_time = 10;
        msg->last_member_query_interval = 1000;
        msg->version = 2;
        msg->afi = MLD_IP_IPV4_AFI;
        msg->vlan_id = vlan_id;
        msg->warm_reboot = WarmStart::isWarmStart() ? 1 : 0;

        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;

            auto tuples = kfvFieldsValues(t);
            auto its = std::find_if(
                tuples.begin(), tuples.end(),
                [](const FieldValueTuple &v){ return v.first == "enabled"; });

            if (its == tuples.end())
            {
                SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s is not enabled", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            for (auto &i : tuples)
            {
                const string &field = fvField(i);
                const string &value = fvValue(i);

                if (field == "enabled")
                {
                    msg->enabled = (value == "true") ? 1 : 0;
                    if (!msg->enabled)
                    {
                        msg->op_code = L2MCD_OP_DISABLE;
                        enabled = false;
                        m_vlanIgmpSnoopMap.erase(vlan_id);
                    }
                    else
                    {
                        enabled = true;
                        m_vlanIgmpSnoopMap[vlan_id] = true;
                    }
                }
                else if (field == "querier") msg->querier = (value == "true") ? 1 : 0;
                else if (field == "fast-leave") msg->fast_leave = (value == "true") ? 1 : 0;
                else if (field == "version") msg->version = stoi(value);
                else if (field == "query-interval") msg->query_interval = stoi(value);
                else if (field == "last-member-query-interval") msg->last_member_query_interval = stoi(value);
                else if (field == "query-max-response-time") msg->query_max_response_time = stoi(value);
            }
            if (!exist_enabled && !enabled)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            msg->count = port_count;
            PORT_ATTR *ports = msg->ports;
            for (uint32_t j = 0 ; j < port_count;j++ )
            {
                ports[j].tagged = port_list[j].tagged;
                memcpy(ports[j].pnames, port_list[j].pnames, L2MCD_IFNAME_SIZE);
            }
        }
        else
        {
            if (!exist_enabled)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            msg->op_code = L2MCD_OP_DISABLE;
            m_vlanIgmpSnoopMap.erase(vlan_id);
        }

        SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s [key:%s] vlan:%d Ver:%d Qr:%d qi:%d lmqi:%d qmr:%d FL:%d count:%u afi:%d warm-reboot:%d",
            op.c_str(), key.c_str(), vlan_id, msg->version, msg->querier, msg->query_interval, msg->last_member_query_interval,
            msg->query_max_response_time, msg->fast_leave, msg->count, msg->afi,msg->warm_reboot);

        sendMsgL2Mcd(L2MCD_SNOOP_CONFIG_MSG, msg_len, (void *)msg);

        it = consumer.m_toSync.erase(it);
    }
}


void L2McMgr::doL2McMldGlobalTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t  msg_len = 0 ;
    L2MCD_CONFIG_MSG *msg = NULL;
    vector<string> l2mcLocalMemKeys;
    vector<string> l2mcLocalMrouterKeys;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op  = kfvOp(t);
        string vlanKey = key.substr(4); // Remove "Vlan"
        int vlan_id = stoi(vlanKey);

        bool enabled = false;
        bool exist_enabled = false;

        auto vlan_it = m_vlanMldSnoopMap.find(vlan_id);
        if (vlan_it != m_vlanMldSnoopMap.end())
            exist_enabled = true;

        vector<PORT_ATTR> port_list;
        uint32_t port_count = getVlanMembers(key, port_list);

        // header + payload
        msg_len = sizeof(L2MCD_CONFIG_MSG) + (uint32_t)(port_count * sizeof(PORT_ATTR));

        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlan_id);

            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->cmd_code  = 0;
        msg->query_interval = 125;
        msg->query_max_response_time = 10;
        msg->last_member_query_interval = 1000;
        msg->version = 2;
        msg->afi = MLD_IP_IPV6_AFI;
        msg->vlan_id = vlan_id;
        msg->warm_reboot = WarmStart::isWarmStart() ? 1 : 0;
        msg->count = port_count;

        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;

            auto tuples = kfvFieldsValues(t);
            auto its = std::find_if(
                tuples.begin(), tuples.end(),
                [](const FieldValueTuple &v){ return v.first == "enabled"; });

            if (its == tuples.end())
            {
                SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s is not enabled", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            for (auto &i : tuples)
            {
                const string &field = fvField(i);
                const string &value = fvValue(i);

                if (field == "enabled")
                {
                    msg->enabled = (value == "true") ? 1 : 0;
                    if (!msg->enabled)
                    {
                        msg->op_code = L2MCD_OP_DISABLE;
                        enabled = false;
                        m_vlanMldSnoopMap.erase(vlan_id);
                    }
                    else
                    {
                        enabled = true;
                        m_vlanMldSnoopMap[vlan_id] = true;
                    }
                }
                else if (field == "querier") msg->querier = (value == "true") ? 1 : 0;
                else if (field == "fast-leave") msg->fast_leave = (value == "true") ? 1 : 0;
                else if (field == "version") msg->version = stoi(value);
                else if (field == "query-interval") msg->query_interval = stoi(value);
                else if (field == "last-member-query-interval") msg->last_member_query_interval = stoi(value);
                else if (field == "query-max-response-time") msg->query_max_response_time = stoi(value);
            }
            if (!exist_enabled && !enabled)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            PORT_ATTR *ports = msg->ports;
            for (uint32_t j= 0; j < port_count; j++ )
            {
                ports[j].tagged = port_list[j].tagged;
                memcpy(ports[j].pnames, port_list[j].pnames, L2MCD_IFNAME_SIZE);
            }
        }
        else
        {
            if (!exist_enabled)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            msg->op_code = L2MCD_OP_DISABLE;
            m_vlanMldSnoopMap.erase(vlan_id);
        }

        SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s [key:%s] vlan:%d Ver:%d Qr:%d qi:%d lmqi:%d qmr:%d FL:%d count:%u afi:%d warm-reboot:%d",
            op.c_str(), key.c_str(), vlan_id, msg->version, msg->querier, msg->query_interval, msg->last_member_query_interval,
            msg->query_max_response_time, msg->fast_leave, msg->count, msg->afi, msg->warm_reboot);

        sendMsgL2Mcd(L2MCD_SNOOP_CONFIG_MSG, msg_len, (void *)msg);

        it = consumer.m_toSync.erase(it);
    }

}

void L2McMgr::updateMrouterEntry(const string vlan_id, const string ifname)
{
    SWSS_LOG_ENTER();

    vector<string> l2mcIgmpMrouterKeys, l2mcMldMrouterKeys;
    //"L2MC_MROUTER|Vlan10|Ethernet5" |"MLD_L2MC_MROUTER|Vlan10|Ethernet5"
    m_cfgL2McMrouterTable.getKeys(l2mcIgmpMrouterKeys);
    m_cfgL2McMldMrouterTable.getKeys(l2mcMldMrouterKeys);

    vector<pair<string, int>> l2mcMrouterKeys;
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;

    for (auto &k : l2mcIgmpMrouterKeys)
        l2mcMrouterKeys.push_back({k, MLD_IP_IPV4_AFI});
    for (auto &k : l2mcMldMrouterKeys)
        l2mcMrouterKeys.push_back({k, MLD_IP_IPV6_AFI});

    for (auto &item : l2mcMrouterKeys)
    {
        const string &entry = item.first;
        int afi = item.second;

        // entry example: "Vlan10|Ethernet5"
        vector<string> tokens = swss::tokenize(entry, '|');
        if (tokens.size() != 2) {
            SWSS_LOG_ERROR("Invalid MROUTER key: %s", entry.c_str());
            continue;
        }
        SWSS_LOG_NOTICE("Invalid MROUTER key: %s", entry.c_str());

        string vlanStr = tokens[0];
        string iname   = tokens[1];
        SWSS_LOG_NOTICE("MROUTER vlanStr: %s iname : %s", vlanStr.c_str(),iname.c_str());

        
        const string vlanNum = vlanStr.substr(4);
        int vlanid = 0;

        try {
            vlanid = stoi(vlanNum);
        }
        catch (exception &e)
        {
            SWSS_LOG_ERROR("Invalid VLAN number in MROUTER key: %s", vlanStr.c_str());
            continue;
        }


        //SWSS_LOG_INFO("vlanKey %s, vlan_id %s ,iname %s ", vlanKey.c_str(), vlan_id.c_str(), iname.c_str());

        if (!vlan_id.empty() && vlanNum != vlan_id)
            continue;
        if (!ifname.empty()  && iname  != ifname)
            continue;

        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlanid);

            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->vlan_id = vlanid;
        msg->count   = 1;
        msg->afi     = afi;
        msg->op_code = L2MCD_OP_ENABLE;
        PORT_ATTR *ports = msg->ports;

        memcpy(ports[0].pnames, ifname.c_str(), L2MCD_IFNAME_SIZE);

        SWSS_LOG_NOTICE("L2MCD_CFG:MROUTER: [key:%s] port:%s vlan:%d afi:%d",entry.c_str(), ports[0].pnames, msg->vlan_id, msg->afi);
        sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, msg_len, (void *)msg);
    }
}

void  L2McMgr::doL2McMrouterUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {   
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        size_t pos = key.find('|');
        string vlanKey = key.substr(4,pos-4); 
        string iname  = key.substr(pos+1);
        int vlan_id = stoi(vlanKey.c_str());
        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlan_id);

            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->vlan_id = vlan_id;
        msg->count=1;
        msg->afi = MLD_IP_IPV4_AFI;
        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_DEBUG("MemIntf: %s", iname.c_str());
        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;
        }
        else
        {
            msg->op_code = L2MCD_OP_DISABLE;
        }
        for (auto i : kfvFieldsValues(t))
        {
            SWSS_LOG_INFO("L2MCD_CFG Field: %s Val %s vlan:%d opcode:%d", fvField(i).c_str(), fvValue(i).c_str(), vlan_id, msg->op_code );
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:MROUTER: op:%s [key:%s] %s vlan:%d", op.c_str(), key.c_str(),ports[0].pnames,vlan_id);
        sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, msg_len, (void *)msg);
        it = consumer.m_toSync.erase(it);
    }

}

void  L2McMgr::doL2McMldMrouterUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        size_t pos = key.find('|');
        string vlanKey = key.substr(4,pos-4); 
        string iname  = key.substr(pos+1);
        int vlan_id = stoi(vlanKey.c_str());
        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlan_id);

            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->vlan_id = vlan_id;
        msg->count=1;
        msg->afi = MLD_IP_IPV6_AFI;
        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_DEBUG("MemIntf: %s", iname.c_str());
        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;
        }
        else
        {
            msg->op_code = L2MCD_OP_DISABLE;
        }
        for (auto i : kfvFieldsValues(t))
        {
            SWSS_LOG_INFO("L2MCD_CFG Field: %s Val %s vlan:%d opcode:%d", fvField(i).c_str(), fvValue(i).c_str(), vlan_id, msg->op_code );
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:MROUTER: op:%s [key:%s] %s vlan:%d afi:%d", op.c_str(), key.c_str(),ports[0].pnames,vlan_id,msg->afi);
        sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, msg_len , (void *)msg);
        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::updateGrpStaticEntry(const string vlan_id, const string ifname)
{
    SWSS_LOG_ENTER();

    vector<string> l2mcGrpStaticKeys, l2mcIgmpGrpStaticKeys , l2mcMldStaticKeys;
    string srcipv4="0.0.0.0";
    string srcipv6="0::0";
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;

    m_cfgL2McStaticTable.getKeys(l2mcIgmpGrpStaticKeys);
    m_cfgL2McMldStaticTable.getKeys(l2mcMldStaticKeys);
    l2mcGrpStaticKeys.insert(l2mcGrpStaticKeys.end(), l2mcIgmpGrpStaticKeys.begin(), l2mcIgmpGrpStaticKeys.end());
    l2mcGrpStaticKeys.insert(l2mcGrpStaticKeys.end(), l2mcMldStaticKeys.begin(), l2mcMldStaticKeys.end());

    for (const auto &entry : l2mcGrpStaticKeys)
    {
        SWSS_LOG_NOTICE("StaticEntry entry");
        vector<string> tokens = swss::tokenize(entry, '|'); 

        string vlanKey = tokens[0];
        string ipKey   = tokens[1];
        string iname   = tokens[2];
        SWSS_LOG_NOTICE("StaticEntry entry vlanKey %s ipKey %s iname %s",vlanKey.c_str(),ipKey.c_str(),iname.c_str());

        const string vlanNum = vlanKey.substr(4);
        int vlanid = 0;

        try {
            vlanid = stoi(vlanNum);
        }
        catch (exception &e)
        {
            SWSS_LOG_ERROR("Invalid VLAN number in MROUTER key: %s", vlanNum.c_str());
            continue;
        }


        if (!vlan_id.empty() && vlanNum != vlan_id)
            continue;
        if (!ifname.empty()  && iname   != ifname)
            continue;

        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlanid);

            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->op_code = L2MCD_OP_ENABLE;
        msg->vlan_id = vlanid;
        msg->count   = 1;

        memcpy(msg->gaddr, ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);

        if (IpAddress(ipKey).isV4())
        {
            msg->afi = MLD_IP_IPV4_AFI;
            memcpy(msg->saddr, srcipv4.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        }
        else
        {
            msg->afi = MLD_IP_IPV6_AFI;
            memcpy(msg->saddr, srcipv6.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        }

        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_DEBUG("MemIntf: %s", iname.c_str());

        SWSS_LOG_NOTICE("L2MCD_CFG:STATIC [key:%s] GA:%s Port:%s vlan:%d afi:%d", entry.c_str(), ipKey.c_str(), iname.c_str(), msg->vlan_id, msg->afi);

        sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, msg_len, (void *)msg);
    }
}

void L2McMgr::doL2McStaticEntryTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    string srcip="0.0.0.0";
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        size_t pos = key.find('|');
        string vlanKey = key.substr(4,pos-4); 
        string key2 = key.substr(pos+1);
        pos = key2.find('|');
        string ipKey = key2.substr(0,pos);
        string iname = key2.substr(pos+1);
        int vlan_id = stoi(vlanKey.c_str());
        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlan_id);

            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;
            size_t idx = key.find('|');
            string tmp_key = key.substr(0, idx+1) + srcip + key.substr(idx);
        }
        else
        {
            msg->op_code = L2MCD_OP_DISABLE;
        }
        msg->cmd_code=0;
        msg->vlan_id = vlan_id;
        msg->count=1;
        
        memcpy(msg->gaddr,ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        memcpy(msg->saddr,srcip.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        msg->afi = MLD_IP_IPV4_AFI;

        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_DEBUG("MemIntf: %s", iname.c_str());

        for (auto i : kfvFieldsValues(t))
        {
            SWSS_LOG_INFO("L2MCD_CFG Field: %s Val %s vlan:%d opcode:%d", fvField(i).c_str(), fvValue(i).c_str(), vlan_id, msg->op_code );
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:STATIC op:%s [key:%s]  GA:%s Port:%s vlan:%d", op.c_str(), key.c_str(), ipKey.c_str(), iname.c_str(), vlan_id);
        sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, msg_len, (void *)msg);
        it = consumer.m_toSync.erase(it);
    }

}

void L2McMgr::doL2McMldStaticEntryTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    string srcip="0::0";
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        size_t pos = key.find('|');
        string vlanKey = key.substr(4,pos-4); 
        string key2 = key.substr(pos+1);
        pos = key2.find('|');
        string ipKey = key2.substr(0,pos);
        string iname = key2.substr(pos+1);
        int vlan_id = stoi(vlanKey.c_str());
        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlan_id);

            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;
        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;
        }
        else
        {
            msg->op_code = L2MCD_OP_DISABLE;
        }
        msg->cmd_code=0;
        msg->vlan_id = vlan_id;
        msg->count=1;
        msg->afi = MLD_IP_IPV6_AFI;
        memcpy(msg->gaddr,ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        memcpy(msg->saddr,srcip.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_DEBUG("MemIntf: %s", iname.c_str());
        for (auto i : kfvFieldsValues(t))
        {
            SWSS_LOG_INFO("L2MCD_CFG Field: %s Val %s vlan:%d opcode:%d", fvField(i).c_str(), fvValue(i).c_str(), vlan_id, msg->op_code );
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:STATIC op:%s [key:%s]  GA:%s Port:%s vlan:%d afi %d", op.c_str(), key.c_str(), ipKey.c_str(), iname.c_str(), vlan_id, msg->afi);
        sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, msg_len, (void *)msg);
        
        it = consumer.m_toSync.erase(it);
    }

}
void L2McMgr::sendL2McSnoopConfig(
        const std::string &key,
        int vlan_id,
        int afi,
        const std::string &op,
        const std::vector<FieldValueTuple> &tuples)
{

    std::vector<PORT_ATTR> port_list;
    uint32_t port_count = getVlanMembers(key, port_list);

    L2MCD_CONFIG_MSG *msg = NULL;
    uint32_t msg_len = sizeof(L2MCD_CONFIG_MSG);
    msg_len += (uint32_t)(port_count * sizeof(PORT_ATTR));
    if (msg_len > L2MCD_MAX_SIZE)
    {
        SWSS_LOG_ERROR(
            "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
            msg_len, L2MCD_MAX_SIZE, vlan_id);

        return;
    }
    memset(buffer, 0, msg_len);
    msg = (L2MCD_CONFIG_MSG *)buffer;

    msg->afi = afi;
    msg->vlan_id = vlan_id;
    msg->cmd_code = 0;
    msg->query_interval = 125;
    msg->query_max_response_time = 10;
    msg->last_member_query_interval = 1000;
    msg->version = 2;
    msg->count = port_count;
    msg->warm_reboot = WarmStart::isWarmStart() ? 1 : 0;

    if (op == SET_COMMAND)
    {
        msg->op_code = L2MCD_OP_ENABLE;

        for (auto &i : tuples)
        {
            const std::string &field = fvField(i);
            const std::string &value = fvValue(i);

            if (field == "enabled") msg->enabled = (value == "true") ? 1 : 0;
            else if (field == "querier") msg->querier = (value == "true") ? 1 : 0;
            else if (field == "fast-leave") msg->fast_leave = (value == "true") ? 1 : 0;
            else if (field == "version") msg->version = stoi(value);
            else if (field == "query-interval") msg->query_interval = stoi(value);
            else if (field == "last-member-query-interval") msg->last_member_query_interval = stoi(value);
            else if (field == "query-max-response-time") msg->query_max_response_time = stoi(value);
        }
    }
    else
    {
        msg->op_code = L2MCD_OP_DISABLE;
    }

    PORT_ATTR *ports = msg->ports;
    for (uint32_t j = 0 ; j < port_count;j++ )
    {
        ports[j].tagged = port_list[j].tagged;
        memcpy(ports[j].pnames, port_list[j].pnames, L2MCD_IFNAME_SIZE);
        SWSS_LOG_INFO("L2MCD_CFG:SNOOP vlan %s mem-port:%s idx:%u total:%u",
                    key.c_str(), ports[j].pnames, j, port_count);
    }

    SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s [key:%s] vlan:%d Ver:%d Qr:%d qi:%d lmqi:%d qmr:%d FL:%d count:%u afi:%d",
                   op.c_str(), key.c_str(), vlan_id, msg->version, msg->querier, msg->query_interval,
                   msg->last_member_query_interval, msg->query_max_response_time,
                   msg->fast_leave, msg->count, msg->afi);

    sendMsgL2Mcd(L2MCD_SNOOP_CONFIG_MSG, msg_len, (void *)msg);

}

void L2McMgr::doVlanUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op  = kfvOp(t);

        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            return;
        }

        string vlanKey = key.substr(4);
        int vlan_id = stoi(vlanKey);

        bool has_IGMP = false;
        bool has_MLD = false;

        std::vector<FieldValueTuple> tuples_IGMP;
        std::vector<FieldValueTuple> tuples_MLD;

        /* IGMP snooping */
        if (m_cfgL2McGlobalTable.get(key, tuples_IGMP))
        {
            auto it_en = std::find_if(
                tuples_IGMP.begin(), tuples_IGMP.end(),
                [](auto &t){ return t.first == "enabled"; });

            if (it_en != tuples_IGMP.end() && fvValue(*it_en) == "true")
                has_IGMP = true;
        }

        /* MLD snooping */
        if (m_cfgL2McMldGlobalTable.get(key, tuples_MLD))
        {
            auto it_en = std::find_if(
                tuples_MLD.begin(), tuples_MLD.end(),
                [](auto &t){ return t.first == "enabled"; });

            if (it_en != tuples_MLD.end() && fvValue(*it_en) == "true")
                has_MLD = true;
        }

        if (!has_IGMP && !has_MLD)
        {
            SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s has no IPv4 nor IPv6 snooping enabled", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        if (has_IGMP)
            sendL2McSnoopConfig(
                key, vlan_id, MLD_IP_IPV4_AFI, op, tuples_IGMP);

        if (has_MLD)
            sendL2McSnoopConfig(
                key, vlan_id, MLD_IP_IPV6_AFI, op, tuples_MLD);

        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::ipcInitL2McMgr()
{
    int ret;
	struct sockaddr_un addr;

    unlink(L2MCMGR_IPC_SOCK_NAME);
    l2mcd_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (l2mcd_fd < 0)
    {
		SWSS_LOG_ERROR("socket error %s", strerror(errno));
		return;
    }

    // setup socket address structure
    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, L2MCMGR_IPC_SOCK_NAME, sizeof(addr.sun_path)-1);

    ret = (int)bind(l2mcd_fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un));
    if (ret == -1)
    {
		SWSS_LOG_ERROR("ipc bind error %s", strerror(errno));
        close(l2mcd_fd);
        return;
    }
}


int L2McMgr::sendMsgL2Mcd(L2MCD_MSG_TYPE msgType, uint32_t msgLen, void *data)
{
    L2MCD_IPC_MSG *tx_msg;
    size_t len = 0;
    struct sockaddr_un addr;
    int rc;
    len = msgLen + (offsetof(struct L2MCD_IPC_MSG, data));
    tx_msg = (L2MCD_IPC_MSG *)calloc(1, len);
    if (tx_msg == NULL)
    {
		SWSS_LOG_ERROR("tx_msg mem alloc error\n");
        return -1;
    }

    tx_msg->msg_type = msgType;
    tx_msg->msg_len  = msgLen;
    memcpy(tx_msg->data, data, msgLen);

    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, L2MCD_IPC_SOCK_NAME, sizeof(addr.sun_path)-1);

    rc = (int)sendto(l2mcd_fd, (void*)tx_msg, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == -1)
    {
		SWSS_LOG_ERROR("tx_msg send error type:%d len%d",msgType,(int)len);
    }
    SWSS_LOG_NOTICE("sendMsgL2Mcd tx_msg send type:%d len%d",msgType,(int)len);
    free(tx_msg);
    return rc;
}

/* To check the port init is done or not */
int L2McMgr::isPortInitComplete(DBConnector *app_db)
{
    bool portInit = 0;
    long cnt = 0;

    while(!portInit) {
        Table portTable(app_db, APP_PORT_TABLE_NAME);
        std::vector<FieldValueTuple> tuples;
        portInit = portTable.get("PortInitDone", tuples);

        if(portInit)
            break;
        sleep(1);
        cnt++;
    }
    sleep(5);
    SWSS_LOG_NOTICE("PORT_INIT_DONE : %d %ld", portInit, cnt);
    return portInit;
}


int L2McMgr::getPortOperState(string if_name)
{
    vector<FieldValueTuple> fvs;
    string oper_status;
    string oper_up="up";

    if (if_name.find("Ethernet") != string::npos)
        m_appPortTable.get(if_name.c_str(), fvs);
    else 
        m_appLagTable.get(if_name.c_str(), fvs);
    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) 
    {
         return fv.first == "oper_status";
    });

    if (it != fvs.end())
    {
        oper_status = it->second;
    }
    SWSS_LOG_NOTICE("%s oper:%s ",if_name.c_str(), oper_status.c_str());
    if (oper_status.compare(oper_up) == 0) return 1;
    return 0;
}

int  L2McMgr::getL2McPortList(DBConnector *state_db)
{   
    SWSS_LOG_ENTER();
    Table m_statePortTable(state_db, STATE_PORT_TABLE_NAME);
    Table m_stateLagTable(state_db, STATE_LAG_TABLE_NAME); 
    string Pname; 
    vector<string> portKeys;
    vector<string> lagKeys;
    m_statePortTable.getKeys(portKeys);
    m_stateLagTable.getKeys(lagKeys);
    uint32_t port_count=(unsigned int) portKeys.size();
    port_count+=(unsigned int) lagKeys.size();

    L2MCD_CONFIG_MSG *msg = NULL;
    uint32_t msg_len = sizeof(L2MCD_CONFIG_MSG);
    msg_len += (uint32_t)(port_count * sizeof(PORT_ATTR));
    if (msg_len > L2MCD_MAX_SIZE)
    {
        SWSS_LOG_ERROR(
            "L2MCD_CFG:SNOOP payload too large: %u (max %u)",
            msg_len, L2MCD_MAX_SIZE);
        return -1;
    }
    memset(buffer, 0, msg_len);
    msg = (L2MCD_CONFIG_MSG *)buffer;
    msg->count = port_count;
    int i = 0;
    PORT_ATTR *ports = msg->ports;
    for (auto port_name : portKeys)
    {
        ports[i].oper_state = getPortOperState(port_name);
        memcpy(ports[i++].pnames, port_name.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_INFO("Port:%s oper:%d", port_name.c_str(), ports[i-1].oper_state);
    }
    for (auto lag_name : lagKeys)
    {
        ports[i].oper_state = getPortOperState(lag_name);
        memcpy(ports[i++].pnames, lag_name.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_INFO("Port:%s oper:%d", lag_name.c_str(), ports[i-1].oper_state);
    }
    msg->op_code = L2MCD_OP_ENABLE;
    SWSS_LOG_NOTICE("L2MCD_CFG:PORTLIST count:%d ",  msg->count);
    sendMsgL2Mcd(L2MCD_SNOOP_PORT_LIST_MSG, msg_len, (void *)msg);
    return 0;
}

int L2McMgr::getL2McCfgParams(DBConnector *conf_db)
{
    L2MCD_CONFIG_MSG msg;
    MacAddress m_macAddr;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    int loglevel;

    Table table(conf_db, "DEVICE_METADATA");
    std::vector<FieldValueTuple> ovalues;
    table.get("localhost", ovalues);
    auto it = std::find_if( ovalues.begin(), ovalues.end(), [](const FieldValueTuple& t){ return t.first == "mac";} );
    if ( it == ovalues.end() ) {
        throw runtime_error("couldn't find MAC address of the device from config DB");
    }
    m_macAddr = MacAddress(it->second);
    memcpy(&msg.mac_addr[0], m_macAddr.getMac(), 6);
    loglevel = swss::Logger::getInstance().getMinPrio();
    msg.count=loglevel;
    SWSS_LOG_NOTICE("L2MCD_CFG:PARAMS  loglevel:%d mac:%s %x:%x:%x:%x:%x:%x ", loglevel, m_macAddr.to_string().c_str(), 
        msg.mac_addr[0],msg.mac_addr[1],msg.mac_addr[2],msg.mac_addr[3],msg.mac_addr[4],msg.mac_addr[5]);
    sendMsgL2Mcd(L2MCD_CONFIG_PARAMS_MSG, sizeof(msg), (void *)&msg);
    return 0;
}

int  L2McMgr::getVlanMembers(const string &vlanKey, vector<PORT_ATTR>&port_list)
{
    PORT_ATTR port_id;
    vector<FieldValueTuple> vmEntry;
    vector<string> vmKeys;
    SWSS_LOG_ENTER();
    m_stateVlanMemberTable.getKeys(vmKeys);

   
    for (auto key : vmKeys)
    {
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR); //split VLAN and interface

        string vlanName;
        string intfName;
        if (found != string::npos)
        {
            vlanName = key.substr(0, found);
            intfName = key.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid Key: %s", key.c_str());
            continue;
        }

        if (vlanKey == vlanName)
        {
            strncpy(port_id.pnames, intfName.c_str(), L2MCD_IFNAME_SIZE-1);
            vector<FieldValueTuple> tupEntry;
            string vlanmemkey = vlanKey + "|" + intfName;

            if (m_cfgVlanMemberTable.get(vlanmemkey, tupEntry))
            {
                auto tag = std::find_if(
                    tupEntry.begin(), tupEntry.end(),
                    [](auto &t){ return t.first == "tagging_mode"; });

                if (tag != tupEntry.end() && fvValue(*tag) == "untagged")
                    port_id.tagged = UNTAGGED;
                else
                    port_id.tagged = TAGGED;
            }
            port_list.push_back(port_id);
            SWSS_LOG_INFO("vlan:%s MemIntf: %s", vlanName.c_str(), intfName.c_str());
        }
    }
    SWSS_LOG_INFO("vlan members Key: %s memcnt:%d", vlanKey.c_str(), (int)port_list.size());
    return (int)port_list.size();

}

void L2McMgr::updateVlanMember(const string vlan_id)
{
    vector<string> l2mcVlanMemKeys;
    // Table vlanmemtable(state_db, STATE_VLAN_MEMBER_TABLE_NAME);
    m_stateVlanMemberTable.getKeys(l2mcVlanMemKeys);
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;

    for (auto l2mc_localMemName : l2mcVlanMemKeys)
    {
        size_t found = l2mc_localMemName.find(CONFIGDB_KEY_SEPARATOR); //split VLAN and interface

        string vlanName;
        string intfName;
        if (found != string::npos)
        {
            vlanName = l2mc_localMemName.substr(4, found-4);
            intfName = l2mc_localMemName.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid Key: %s", l2mc_localMemName.c_str());
            continue;
        }
        SWSS_LOG_INFO("VlanName %s, vlan_id %s", vlanName.c_str(), vlan_id.c_str());
        if (vlanName != vlan_id)
        {
            SWSS_LOG_INFO("Vlan not match, continue");
            continue;
        }
        int vlanid = atoi(vlanName.c_str());
        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                msg_len, L2MCD_MAX_SIZE, vlanid);
                
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        msg->vlan_id = vlanid;
        msg->count =1;

        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, intfName.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_DEBUG("MemIntf: %s", intfName.c_str());
        msg->op_code = L2MCD_OP_ENABLE;
        vector<FieldValueTuple> tupEntry;
        if (m_cfgVlanMemberTable.get(l2mc_localMemName, tupEntry))
        {
            auto tag  = std::find_if(
            tupEntry.begin(), tupEntry.end(),
            [](auto &t){ return t.first == "tagging_mode"; });

            if (tag != tupEntry.end() && fvValue(*tag) == "untagged")
                ports[0].tagged = UNTAGGED;
            else
                ports[0].tagged = TAGGED; 
        }
        sendMsgL2Mcd(L2MCD_VLAN_MEM_TABLE_UPDATE, msg_len, (void *)msg);
    }
}

void L2McMgr::doL2McVlanMemUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);
        size_t found = key.find("|");
        string vlanName = key.substr(4, found-4);
        string intfName = key.substr(found+1);

        SWSS_LOG_INFO("vlanmemebr key:%s op:%s", key.c_str(), op.c_str());
        if ((found != string::npos))
        {
            int vlanid = stoi(vlanName.c_str());
            msg_len = sizeof(L2MCD_CONFIG_MSG) +  sizeof(PORT_ATTR);
            if (msg_len > L2MCD_MAX_SIZE)
            {
                SWSS_LOG_ERROR(
                    "L2MCD_CFG:SNOOP payload too large: %u (max %u), vlan=%d",
                    msg_len, L2MCD_MAX_SIZE, vlanid);
                it = consumer.m_toSync.erase(it);
                continue;
            }
            memset(buffer, 0, msg_len);
            msg = (L2MCD_CONFIG_MSG *)buffer;
            msg->vlan_id = vlanid;
            msg->count =1;
            PORT_ATTR *ports = msg->ports;
            memcpy(ports[0].pnames, intfName.c_str(), L2MCD_IFNAME_SIZE);
            SWSS_LOG_DEBUG("MemIntf: %s", intfName.c_str());
            vector<FieldValueTuple> tupEntry;
            if (m_cfgVlanMemberTable.get(key, tupEntry))
            {
                auto tag  = std::find_if(
                tupEntry.begin(), tupEntry.end(),
                [](auto &t){ return t.first == "tagging_mode"; });

                if (tag != tupEntry.end() && fvValue(*tag) == "untagged")
                    ports[0].tagged = UNTAGGED;
                else
                    ports[0].tagged = TAGGED; 
            }
            if (op == SET_COMMAND)
            {
                msg->op_code = L2MCD_OP_ENABLE;
            }
            else
            {
                msg->op_code = L2MCD_OP_DISABLE;
            }
            SWSS_LOG_NOTICE("L2MCD_CFG:VLAN_MEMBER op:%s iname:%s %s vlan:%d tagged:%d", op.c_str(), ports[0].pnames, intfName.c_str(), msg->vlan_id, ports[0].tagged);
            sendMsgL2Mcd(L2MCD_VLAN_MEM_TABLE_UPDATE, msg_len, (void *)msg);
        }
        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::doL2McLagMemberUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);

        string po_name;
        string po_mem;
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);
        msg_len = sizeof(L2MCD_CONFIG_MSG) + 2 * sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u)",
                msg_len, L2MCD_MAX_SIZE);
            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        if (found != string::npos)
        {   
            PORT_ATTR *ports = msg->ports;
            po_name = key.substr(0, found);
            po_mem  = key.substr(found+1);
            SWSS_LOG_INFO("LAG_MEMBER %s %s %s", po_name.c_str(), po_mem.c_str(), op.c_str());
            memcpy(ports[0].pnames, po_name.c_str(), L2MCD_IFNAME_SIZE);
            memcpy(ports[1].pnames, po_mem.c_str(), L2MCD_IFNAME_SIZE);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            msg->op_code = L2MCD_OP_ENABLE;
        }
        else
        {
            msg->op_code = L2MCD_OP_DISABLE;
        }
        msg->count = 2;
        SWSS_LOG_NOTICE("L2MCD_CFG:LAG_MEMBER op:%s  Po:%s  count:%d", op.c_str(), po_name.c_str(), msg->count);
        sendMsgL2Mcd(L2MCD_LAG_MEM_TABLE_UPDATE, msg_len, (void *)msg);
        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::doL2McL3InterfaceUpdateTask(Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    //Vlan700|100.100.100.2/24   -- 700 , 100.100.100.2, 24
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);
        size_t found = key.find("|");
        size_t found2 =  key.find("/");
        SWSS_LOG_INFO(" key:%s op:%s", key.c_str(), op.c_str());
        if (!key.find("Vlan",0)  && (found != string::npos) && (found2 != string::npos))
        {
            string vlanstr = key.substr(4, found-4);
            int vlanid = stoi(vlanstr.c_str());

            string ipstr = key.substr(found+1, found2-found-1);
            string prefix = key.substr(found2+1);
            int pr_len = stoi(prefix);
            msg.vlan_id =vlanid;
            memcpy(msg.gaddr,ipstr.c_str(), L2MCD_IP_ADDR_STR_SIZE);
            msg.prefix_length = pr_len;
            SWSS_LOG_NOTICE("L2MCD_CFG:INTERFACE %s Vid:%s %d ip:%s prefix:%s(%d)", op.c_str(), vlanstr.c_str(),  msg.vlan_id, msg.gaddr, prefix.c_str(),msg.prefix_length);
            if (ipstr.find(':') != string::npos)
            {
                msg.afi = MLD_IP_IPV6_AFI;
                SWSS_LOG_DEBUG("IPv6 address: %s", ipstr.c_str());
            }
            else if (ipstr.find('.') != string::npos)
            {
                msg.afi = MLD_IP_IPV4_AFI;
                SWSS_LOG_DEBUG("IPv4 address: %s", ipstr.c_str());
            }
            else
            {
                msg.afi = MLD_IP_IPV4_AFI;
                SWSS_LOG_WARN("Cannot determine address family for IP: %s, defaulting to IPv4", ipstr.c_str());
            }
            if (op == SET_COMMAND)
            {
                msg.op_code = L2MCD_OP_ENABLE;
            }
            else
            {
                msg.op_code = L2MCD_OP_DISABLE;
            }
        
            sendMsgL2Mcd(L2MCD_INTERFACE_TABLE_UPDATE, sizeof(msg), (void *)&msg);
        }
        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::doL2McInterfaceUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
        if (msg_len > L2MCD_MAX_SIZE)
        {
            SWSS_LOG_ERROR(
                "L2MCD_CFG:SNOOP payload too large: %u (max %u)",
                msg_len, L2MCD_MAX_SIZE);
            it = consumer.m_toSync.erase(it);
            continue;
        }
        memset(buffer, 0, msg_len);
        msg = (L2MCD_CONFIG_MSG *)buffer;

        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);
        msg->count=1;
        msg->op_code = (op == SET_COMMAND)? L2MCD_OP_ENABLE:L2MCD_OP_DISABLE;
        PORT_ATTR *ports = msg->ports;
        memcpy(ports[0].pnames, key.c_str(), L2MCD_IFNAME_SIZE);
        ports[0].oper_state = getPortOperState(ports[0].pnames);
        SWSS_LOG_NOTICE("L2MCD_CFG: IF:%s op:%s oper:%d", key.c_str(), op.c_str(),ports[0].oper_state);
        sendMsgL2Mcd(L2MCD_SNOOP_PORT_LIST_MSG, msg_len, (void *)msg);
        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::doL2McSuppressUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple &tuple = it->second;

        string op = kfvOp(tuple);
        string vlan_name = kfvKey(tuple);
        SWSS_LOG_NOTICE("doL2McSuppressUpdateTask : enter SET_COMMAND");
        if (op == SET_COMMAND)
        {
            std::vector<FieldValueTuple> validFields;
            for (const auto &fv : kfvFieldsValues(tuple))
            {
                const string &field = fvField(fv);
                const string &value = fvValue(fv);

                if (field == "ipv4-optimised-multicast-flood" || field == "ipv4-link-local-groups-suppression" ||field == "ipv6-optimised-multicast-flood" || field == "ipv6-link-local-groups-suppression")
                {
                    validFields.push_back(fv);
                    SWSS_LOG_NOTICE("doL2McSuppressUpdateTask : Set %s: %s = %s", vlan_name.c_str(), field.c_str(), value.c_str());
                }
                else
                {
                    SWSS_LOG_WARN("Unknown field '%s' for VLAN '%s', ignoring", field.c_str(), vlan_name.c_str());
                }
            }

            if (!validFields.empty())
            {
                m_appL2mcSuppressTableProducer.set(vlan_name, validFields);
            }
            else
            {
                SWSS_LOG_WARN("No valid fields to set for VLAN '%s'", vlan_name.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("doL2McSuppressUpdateTask Deleting L2MC suppression config for VLAN '%s'", vlan_name.c_str());
            m_appL2mcSuppressTableProducer.del(vlan_name);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation '%s' for VLAN '%s'", op.c_str(), vlan_name.c_str());
        }

        it = consumer.m_toSync.erase(it);  // Remove processed entry
    }
}

void L2McMgr::doL2McProcRemoteEntries(string op, string key, string key_seperator, int cmd_type)
{
    SWSS_LOG_ENTER();
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
    if (msg_len > L2MCD_MAX_SIZE)
    {
        SWSS_LOG_ERROR(
            "L2MCD_CFG:SNOOP payload too large: %u (max %u)",
            msg_len, L2MCD_MAX_SIZE);
        return;
    }
    memset(buffer, 0, msg_len);
    msg = (L2MCD_CONFIG_MSG *)buffer;

    size_t pos=key.find(key_seperator.c_str());
    auto vlan_name = key.substr(0,pos);
    auto pos1 = key.find(key_seperator.c_str(), pos+1);
    auto saddr = key.substr(pos+1, pos1-pos-1);
    auto pos2 = key.find(key_seperator.c_str(), pos1+1);
    auto gaddr = key.substr(pos1+1, pos2-pos1-1);
    auto pos3 = key.find(key_seperator.c_str(), pos2+1);
    auto iname = key.substr(pos2+1, pos3-pos2-1);
    auto pos4 = key.find(key_seperator.c_str(), pos3+1);
    auto type = key.substr(pos3+1, pos4-pos3-1);
    auto pos5 = key.find(key_seperator.c_str(), pos4+1);
    auto leave = key.substr(pos4+1, pos5-pos4-1);
    msg->vlan_id = (unsigned int) stoi(vlan_name.substr(4));
    memcpy(msg->saddr,saddr.c_str(), L2MCD_IP_ADDR_STR_SIZE);
    memcpy(msg->gaddr,gaddr.c_str(), L2MCD_IP_ADDR_STR_SIZE);
    if (IpAddress(gaddr).isV4())
    {
        msg->afi = MLD_IP_IPV4_AFI;
    }
    else
    {
        msg->afi = MLD_IP_IPV6_AFI;
    }
    msg->count=1;
    PORT_ATTR *ports = msg->ports;
    memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);

    if (IpAddress(gaddr).isV4() && saddr != "0.0.0.0")
    {
        msg->version = IGMP_VERSION_3;
    }
    else if (IpAddress(gaddr).isV4() && saddr == "0.0.0.0")
    {
        msg->version = IGMP_VERSION_2;
    }
    else if (!IpAddress(gaddr).isV4() && saddr != "0000:0000:0000:0000:0000:0000:0000:0000")
    {
        msg->version = MLD_VERSION_2;
    }
    else
    {
        msg->version = MLD_VERSION_1;
    }

    if (op =="SET")
    {
        msg->op_code = L2MCD_OP_ENABLE;
    }
    else 
    {
        msg->op_code = L2MCD_OP_DISABLE;
    }
    msg->cmd_code=cmd_type;
    SWSS_LOG_NOTICE("L2MCD_CFG:REMOTE op:%s [key:%s]  SA:%s GA:%s Port:%s vlan:%d type:%d", 
           op.c_str(), key.c_str(), saddr.c_str(), gaddr.c_str(), iname.c_str(), msg->vlan_id,msg->cmd_code);
    sendMsgL2Mcd(L2MCD_SNOOP_REMOTE_CONFIG_MSG, msg_len , (void *)msg);
}

void L2McMgr::doL2McProcRemoteMrouterEntries(string op, string key, string key_seperator)
{
    uint32_t msg_len = 0;
    L2MCD_CONFIG_MSG *msg = NULL;
    msg_len = sizeof(L2MCD_CONFIG_MSG) + sizeof(PORT_ATTR);
    if (msg_len > L2MCD_MAX_SIZE)
    {
        SWSS_LOG_ERROR(
            "L2MCD_CFG:SNOOP payload too large: %u (max %u)",
            msg_len, L2MCD_MAX_SIZE);
        return;
    }
    memset(buffer, 0, msg_len);
    msg = (L2MCD_CONFIG_MSG *)buffer;

    SWSS_LOG_ENTER();
    size_t pos=key.find(key_seperator.c_str());
    auto vlan_name = key.substr(0,pos);
    auto pos1 = key.find(key_seperator.c_str(), pos+1);
    auto iname = key.substr(pos+1, pos1-pos-1);
    auto pos2 = key.find(key_seperator.c_str(), pos1+1);
    auto type = key.substr(pos1+1, pos2-pos1-1);
    auto pos3 = key.find(key_seperator.c_str(), pos2+1);
    auto leave = key.substr(pos2+1, pos3-pos2-1);
    auto pos4 = key.find(key_seperator.c_str(), pos3+1);
    auto proctol = key.substr(pos3+1, pos4-pos3-1);
    msg->vlan_id = (unsigned int) stoi(vlan_name.substr(4));
    msg->count=1;
    PORT_ATTR *ports = msg->ports;
    memcpy(ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);

    if(proctol == "V4")
        msg->afi = MLD_IP_IPV4_AFI;
    else
        msg->afi = MLD_IP_IPV6_AFI;

    if (op =="SET")
    {
        msg->op_code = L2MCD_OP_ENABLE;
    }
    else 
    {
        msg->op_code = L2MCD_OP_DISABLE;
    }
    SWSS_LOG_NOTICE("L2MCD_Mrouter:REMOTE op:%s [key:%s] Port:%s vlan:%d", 
           op.c_str(), key.c_str(), iname.c_str(), msg->vlan_id);
    sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_REMOTE_CONFIG_MSG, msg_len , (void *)msg);
}

void L2McMgr::doTask(NotificationConsumer &consumer)
{
    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    if (&consumer == m_l2mcNotificationConsumer)
    {
        SWSS_LOG_INFO("Received l2mc entry notification");
        consumer.pop(op, data, values);
        doL2McProcRemoteEntries(op, data, "|", 0);
    }
    else if (&consumer == m_l2mcMrouterNotificationConsumer)
    {
        SWSS_LOG_INFO("Received l2mc Mrouter notification");
        consumer.pop(op, data, values);
        doL2McProcRemoteMrouterEntries(op, data, "|"); 
    }
    else if (&consumer == m_l2mcCfgparaDoneNotificationConsumer)
    {
        SWSS_LOG_INFO("Received l2mc cfgpara notification");
        consumer.pop(op, data, values);
        SWSS_LOG_INFO("Received l2mcd_sync op  %s data %s",op.c_str(), data.c_str());
        size_t pos = data.find('|');
        if (pos == string::npos) 
        {
            SWSS_LOG_ERROR("Invalid l2mcd_sync data: %s", data.c_str());
            return;
        }
        string option = data.substr(0,pos);
        string param = data.substr(pos+1);
        string vlan_id = "";
        string ifname = "";
        SWSS_LOG_INFO("Received l2mcd_sync op  %s option %s vlan_id %s",op.c_str(), option.c_str(), param.c_str());
        if (op == "SNP")
        {
            vlan_id = param;
            if (option == "enable")
            {
                auto res = m_operUpPorts.insert(vlan_id);
                if (!res.second)
                {
                    SWSS_LOG_INFO("Vlan %s already enable, skip", vlan_id.c_str());
                    return;
                }
                SWSS_LOG_INFO("Vlan %s changed to enable", vlan_id.c_str());
                updateVlanMember(vlan_id);
                updateMrouterEntry(vlan_id, ifname);
                updateGrpStaticEntry(vlan_id, ifname);
            }
            else if (option == "disable")
            {
                SWSS_LOG_INFO("Vlan %s changed to disable", ifname.c_str());
                m_operUpPorts.erase(ifname);
            }
        }
        else if (op == "LINK_STATUS")
        {
            ifname = param;

            if (option == "up")
            {
                auto res = m_operUpPorts.insert(ifname);

                if (!res.second)
                {
                    SWSS_LOG_INFO("Port %s already UP, skip", ifname.c_str());
                    return;
                }
                SWSS_LOG_INFO("Port %s changed to UP", ifname.c_str());
                updateMrouterEntry(vlan_id, ifname);
                updateGrpStaticEntry(vlan_id, ifname);
            }
            else if (option == "down")
            {
                m_operUpPorts.erase(ifname);
            }
        }
            
    }
}
