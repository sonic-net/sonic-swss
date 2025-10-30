/*
 * Copyright 2019 Broadcom. All rights reserved.
 * The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 * 
 * cfgmgr/l2mcmgr.cpp
 */

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

L2McMgr::L2McMgr(DBConnector *confDb, DBConnector *applDb, DBConnector *statDb,
        const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgL2McGlobalTable(confDb, CFG_L2MC_TABLE_NAME),
    m_cfgL2McStaticTable(confDb, CFG_L2MC_STATIC_TABLE_NAME),
    m_cfgL2McMrouterTable(confDb, CFG_L2MC_MROUTER_TABLE_NAME),
    m_stateVlanTable(statDb, STATE_VLAN_TABLE_NAME),
    m_stateVlanMemberTable(statDb, STATE_VLAN_MEMBER_TABLE_NAME),
    m_stateInterfaceTableName(statDb,STATE_INTERFACE_TABLE_NAME),
    m_statel2mcdLocalMemberTable(statDb, STATE_L2MC_MEMBER_TABLE_NAME),
    m_statel2mcdLocalMrouterTable(statDb,STATE_L2MC_MROUTER_TABLE_NAME),
    m_cfgLagMemberTable(confDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_stateLagTable(statDb, STATE_LAG_TABLE_NAME),
    m_appPortTable(applDb, APP_PORT_TABLE_NAME),
    m_appLagTable(applDb,APP_LAG_TABLE_NAME),
    m_appL2mcVlanTable(applDb,APP_L2MC_VLAN_TABLE_NAME),
    m_appL2mcGrpMemTable(applDb,APP_L2MC_MEMBER_TABLE_NAME),
    m_appL2mcMrouterTable(applDb,APP_L2MC_MROUTER_TABLE_NAME),
    m_appL2mcSuppressTableProducer(applDb,APP_L2MC_SUPPRESS_TABLE_NAME),
    l2mcMouterReplayDone(false),
    l2mcGrpMemReplayDone(false),
    l2mcVlanReplayDone(false),
    m_warmstart(false)
{
    SWSS_LOG_ENTER();

    WarmStart::initialize("l2mcmgrd", "l2mcd");
    WarmStart::checkWarmStart("l2mcmgrd", "l2mcd");
    m_warmstart = WarmStart::isWarmStart();
    if (m_warmstart)
    {
        getL2mcVlanEntry(m_l2mcVlanentry);
        getStateGrpMemEntry(m_l2mcGrpMementry);
        getStateMrouterEntry(m_l2mcMrouterentry);
        WarmStart::setWarmStartState("l2mcmgrd", WarmStart::INITIALIZED);
        SWSS_LOG_NOTICE("Starting in warmstart mode");

        vector<string> l2mcVlanKeys, l2mcGrpMemKeys, l2mcMouterKeys;
        m_cfgL2McGlobalTable.getKeys(l2mcVlanKeys);
        m_cfgL2McStaticTable.getKeys(l2mcGrpMemKeys);
        m_cfgL2McMrouterTable.getKeys(l2mcMouterKeys);

        if (l2mcVlanKeys.empty())
        {
            l2mcVlanReplayDone = true;
        }
        if (l2mcGrpMemKeys.empty())
        {
            l2mcGrpMemReplayDone = true;
        }
        if (l2mcMouterKeys.empty())
        {
            l2mcMouterReplayDone = true;
        }
        if (l2mcVlanReplayDone && l2mcGrpMemReplayDone && l2mcMouterReplayDone)
        {
            for (auto itg = m_l2mcGrpMementry.begin(); itg != m_l2mcGrpMementry.end(); itg++)
            {
                removeL2mcGrpMemEntry(itg->first);
            }
            for (auto itm = m_l2mcMrouterentry.begin(); itm != m_l2mcMrouterentry.end(); itm++)
            {
                removeL2mcMrouterEntry(itm->first);
            }
            for (auto its = m_l2mcVlanentry.begin(); its != m_l2mcVlanentry.end(); its++)
            {
                removeL2mcVlanEntry(its->first);
            }

            m_l2mcVlanentry.clear();
            m_l2mcGrpMementry.clear();
            m_l2mcMrouterentry.clear();
            WarmStart::setWarmStartState("l2mcmgrd", WarmStart::RECONCILED);
        }
    }
    else
    {
        WarmStart::setWarmStartState("l2mcmgrd", WarmStart::WSDISABLED);
    }

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

void L2McMgr::getL2mcVlanEntry(map<string, string> &entry)
{
    vector<string> l2mcLocalVlanKeys;
    string id = "";
    m_appL2mcVlanTable.getKeys(l2mcLocalVlanKeys);
    for (auto l2mc_localVlanName : l2mcLocalVlanKeys)
    {
        m_appL2mcVlanTable.hget(l2mc_localVlanName, "id", id);
        entry[l2mc_localVlanName] = id;
    }
}

bool L2McMgr::findL2mcVlanEntry(string key)
{
    for (char &c: key) {if (c=='|') {c=':';}}
    for (auto its = m_l2mcVlanentry.begin(); its != m_l2mcVlanentry.end(); its++)
    {
        if (its->first == key)
            return true;
    }

    return false;
}

void L2McMgr::removeL2mcVlanEntry(string key)
{
    L2MCD_CONFIG_MSG msg;
    int vlan_id = stoi(key.substr(4).c_str());
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    msg.op_code = L2MCD_OP_DISABLE;
    msg.vlan_id = vlan_id;
    sendMsgL2Mcd(L2MCD_SNOOP_CONFIG_MSG, sizeof(msg), (void *)&msg);
    m_appL2mcVlanTable.del(key);

    for (auto its = m_l2mcentry.begin(); its != m_l2mcentry.end(); its++)
    {
        size_t pos = its->first.find("|");
        if (key != its->first.substr(0, pos))
            continue;
        string tmp_str = its->first.substr(pos+1);
        pos = tmp_str.find("|");
        if (pos != std::string::npos)
            removeL2mcGrpMemEntry(its->first);
        else
            removeL2mcMrouterEntry(its->first);

    }
}

void L2McMgr::getStateGrpMemEntry(map<string, string> &entry)
{
    vector<string> l2mcLocalMemKeys;
    string type_str = "";
    m_statel2mcdLocalMemberTable.getKeys(l2mcLocalMemKeys);
    for (auto l2mc_localMemName : l2mcLocalMemKeys)
    {
        m_statel2mcdLocalMemberTable.hget(l2mc_localMemName, "type", type_str);
        if (type_str == "static")
        {
            entry[l2mc_localMemName] = type_str;
            m_l2mcentry[l2mc_localMemName] = type_str;
        }
    }
}

bool L2McMgr::findStateGrpMemEntry(string key)
{
    for (auto its = m_l2mcGrpMementry.begin(); its != m_l2mcGrpMementry.end(); its++)
    {
        if (its->first == key)
            return true;
    }

    return false;
}

void L2McMgr::removeL2mcGrpMemEntry(string key)
{
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    //Vlan100|0.0.0.0|224.10.10.10|Ethernet36
    size_t pos = key.find(CONFIGDB_KEY_SEPARATOR);
    string vlanKey = key.substr(4,pos-4);
    string key2 = key.substr(pos+1);
    pos = key2.find(CONFIGDB_KEY_SEPARATOR);
    string sipKey = key2.substr(0,pos);
    string key3 = key2.substr(pos+1);
    pos = key3.find(CONFIGDB_KEY_SEPARATOR);
    string ipKey = key3.substr(0,pos);
    string iname = key3.substr(pos+1);

    int vlan_id = stoi(vlanKey.c_str());
    msg.op_code = L2MCD_OP_DISABLE;
    msg.vlan_id = vlan_id;
    msg.count=1;
    memcpy(msg.gaddr,ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
    memcpy(msg.saddr,sipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
    memcpy(msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);

    sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, sizeof(msg), (void *)&msg);
    std::vector<FieldValueTuple> fvVector;
    if (m_statel2mcdLocalMemberTable.get(key, fvVector))
        m_statel2mcdLocalMemberTable.del(key);
    for (char &c : key) { if (c == '|') {c = ':';}}
    m_appL2mcGrpMemTable.del(key);
}

void L2McMgr::getStateMrouterEntry(map<string, string> &entry)
{
    vector<string> l2mcLocalMrouterKeys;
    string type_str = "";
    m_statel2mcdLocalMrouterTable.getKeys(l2mcLocalMrouterKeys);
    for (auto l2mc_localMrouterName : l2mcLocalMrouterKeys)
    {
        m_statel2mcdLocalMrouterTable.hget(l2mc_localMrouterName, "type", type_str);
        if (type_str == "static")
        {
            entry[l2mc_localMrouterName] = type_str;
            m_l2mcentry[l2mc_localMrouterName] = type_str;
        }
    }
}

bool L2McMgr::findStateMrouterEntry(string key)
{
    for (auto its = m_l2mcMrouterentry.begin(); its != m_l2mcMrouterentry.end(); its++)
    {
        if (its->first == key)
            return true;
    }

    return false;
}

void L2McMgr::removeL2mcMrouterEntry(string key)
{
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    //Vlan100|Ethernet49
    size_t pos = key.find(CONFIGDB_KEY_SEPARATOR);
    string vlanKey = key.substr(4,pos-4);
    string iname = key.substr(pos+1);

    int vlan_id = stoi(vlanKey.c_str());
    msg.op_code = L2MCD_OP_DISABLE;
    msg.vlan_id = vlan_id;
    msg.count=1;
    memcpy(msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);

    sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, sizeof(msg), (void *)&msg);
    std::vector<FieldValueTuple> fvVector;
    if (m_statel2mcdLocalMrouterTable.get(key, fvVector))
        m_statel2mcdLocalMrouterTable.del(key);
    for (char &c : key) { if (c == '|') {c = ':';}}
    m_appL2mcMrouterTable.del(key);
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
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table.c_str());
    }

    if (m_warmstart && l2mcVlanReplayDone && l2mcGrpMemReplayDone && l2mcMouterReplayDone)
    {
        for (auto itg = m_l2mcGrpMementry.begin(); itg != m_l2mcGrpMementry.end(); itg++)
        {
            removeL2mcGrpMemEntry(itg->first);
        }
        for (auto itm = m_l2mcMrouterentry.begin(); itm != m_l2mcMrouterentry.end(); itm++)
        {
            removeL2mcMrouterEntry(itm->first);
        }
        for (auto its = m_l2mcVlanentry.begin(); its != m_l2mcVlanentry.end(); its++)
        {
            removeL2mcVlanEntry(its->first);
        }

        m_l2mcVlanentry.clear();
        m_l2mcGrpMementry.clear();
        m_l2mcMrouterentry.clear();
        m_l2mcentry.clear();
        /* reset flag to avoid action times */
        m_warmstart = false;
        WarmStart::setWarmStartState("l2mcmgrd", WarmStart::RECONCILED);
    }
}

void L2McMgr::doL2McGlobalTask(Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    int j=0;      
    SWSS_LOG_ENTER();
    vector<string> l2mcLocalMemKeys;
    vector<string> l2mcLocalMrouterKeys;

    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        string vlanKey = key.substr(4); // Remove Vlan prefix
        int vlan_id = stoi(vlanKey.c_str());
        msg.cmd_code=0;
        msg.query_interval=125;
        msg.query_max_response_time=10;
        msg.last_member_query_interval=1000;
        msg.version=2;
        if (op == SET_COMMAND)
        {
            msg.op_code = L2MCD_OP_ENABLE;
            msg.vlan_id = vlan_id;
            auto tuples = kfvFieldsValues(t);
            auto its = std::find_if( tuples.begin(), tuples.end(), [](const FieldValueTuple& v){ return v.first == "enabled";} );
            if ( its == tuples.end()) {
                SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP  %s is not enabled",key.c_str());
                it = consumer.m_toSync.erase(it);
                return;
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "enabled")
                {
                    msg.enabled = (fvValue(i) == "true") ? 1 : 0;
                }
                else if (fvField(i) == "querier")
                {
                    msg.querier = (fvValue(i) == "true") ? 1 : 0;
                }
                else if (fvField(i) == "fast-leave")
                {
                    msg.fast_leave = (fvValue(i) == "true") ? 1 : 0; 
                }
                else if (fvField(i) == "version")
                {
                    msg.version = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "query-interval")
                {
                    msg.query_interval = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "last-member-query-interval")
                {
                    msg.last_member_query_interval = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "query-max-response-time")
                {
                    msg.query_max_response_time = stoi(fvValue(i).c_str());
                }
                vector<PORT_ATTR> port_list;
                msg.count =  getVlanMembers(key,port_list);
                j=0;
                for (auto pentry = port_list.begin(); pentry != port_list.end(); pentry++)
                {
                    memcpy(&msg.ports[j++].pnames, pentry->pnames, L2MCD_IFNAME_SIZE);
                    SWSS_LOG_INFO("L2MCD_CFG:SNOOP vlan %s mem-port:%s idx:%d size:%d", key.c_str(), msg.ports[j-1].pnames, j-1, (int)port_list.size());
                }

                if (m_warmstart && findL2mcVlanEntry(key))
                {
                    (void)m_l2mcVlanentry.erase(key);
                }
            }
        }
        else 
        {
            msg.op_code = L2MCD_OP_DISABLE;
            msg.vlan_id = vlan_id;
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s [key:%s] vlan:%d,Ver:%d, Qry:%d, qI:%d,lmqi:%d,qmr:%d,FL:%d, count:%d", 
               op.c_str(), key.c_str(), vlan_id, msg.version, msg.querier,msg.query_interval,
               msg.last_member_query_interval, msg.query_max_response_time ,
               msg.fast_leave,msg.count);
        sendMsgL2Mcd(L2MCD_SNOOP_CONFIG_MSG, sizeof(msg), (void *)&msg);

        /* sync dynamic member entry to l2mcd */
        string type_str = "";
        int cmd_type = 0;
        m_statel2mcdLocalMemberTable.getKeys(l2mcLocalMemKeys);
        for (auto l2mc_localMemName : l2mcLocalMemKeys)
        {
            m_statel2mcdLocalMemberTable.hget(l2mc_localMemName, "type", type_str);
            if (type_str=="remote")cmd_type = 0;
            if (type_str=="dynamic")cmd_type = 2;
            if (type_str=="static")cmd_type = 3;
            SWSS_LOG_INFO("MEMBER_REPLAY %s %s", l2mc_localMemName.c_str(), type_str.c_str());
            size_t pos=key.find('|');
            auto vlan_name = key.substr(0,pos);
            if (vlan_id == stoi(vlan_name.substr(4)))
            {
                doL2McProcRemoteEntries("SET",l2mc_localMemName,"|", cmd_type);
            }
        }

        /* sync dynamic mrouter entry to l2mcd */
        // string mr_type_str = "";
        // m_statel2mcdLocalMrouterTable.getKeys(l2mcLocalMrouterKeys);
        // for (auto l2mc_localMrouterName : l2mcLocalMrouterKeys)
        // {
        //     m_statel2mcdLocalMrouterTable.hget(l2mc_localMrouterName, "type", mr_type_str);
        //     SWSS_LOG_INFO("MROUTER_REPLAY %s %s", l2mc_localMrouterName.c_str(), mr_type_str.c_str());
        //     if (mr_type_str == "static") continue;
        //     size_t pos=key.find('|');
        //     auto vlan_name = key.substr(0,pos);
        //     if (vlan_id == stoi(vlan_name.substr(4)))
        //     {
        //         doL2McProcRemoteMrouterEntries("SET",l2mc_localMrouterName,"|");
        //     }
        // }
        it = consumer.m_toSync.erase(it);
    }

    if (m_warmstart && consumer.m_toSync.empty())
    {
        l2mcVlanReplayDone = true;
    }

}

void  L2McMgr::updateMrouterEntry(const string vlan_id, const string ifname)
{
    vector<string> l2mcMrouterKeys;
    L2MCD_CONFIG_MSG msg;
    SWSS_LOG_ENTER();
    string srcip="0.0.0.0";
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    // "L2MC_MROUTER|Vlan20|Ethernet36"
    m_cfgL2McMrouterTable.getKeys(l2mcMrouterKeys);
    if (vlan_id != "")
    {
        for (auto l2mc_Mrouter : l2mcMrouterKeys)
        {
            size_t pos = l2mc_Mrouter.find('|');
            string vlanKey = l2mc_Mrouter.substr(4,pos-4);
            string iname  = l2mc_Mrouter.substr(pos+1);
            SWSS_LOG_INFO("vlanKey %s, vlan_id %s", vlanKey.c_str(), vlan_id.c_str());
            if (vlanKey != vlan_id)
            {
                SWSS_LOG_INFO("Vlan not match, continue");
                continue;
            }
            int vlan_id = stoi(vlanKey.c_str());
            msg.vlan_id = vlan_id;
            msg.count=1;
            memcpy(&msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
            msg.op_code = L2MCD_OP_ENABLE;
            SWSS_LOG_NOTICE("L2MCD_CFG:MROUTER: [key:%s] %s vlan:%d", l2mc_Mrouter.c_str(),msg.ports[0].pnames,vlan_id);
            sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, sizeof(msg), (void *)&msg);
        }
    }
    else if (ifname != "")
    {
        for (auto l2mc_Mrouter : l2mcMrouterKeys)
        {
            size_t pos = l2mc_Mrouter.find('|');
            string vlanKey = l2mc_Mrouter.substr(4,pos-4);
            string iname  = l2mc_Mrouter.substr(pos+1);
            SWSS_LOG_INFO("vlanKey %s, vlan_id %s", vlanKey.c_str(), vlan_id.c_str());
            if (iname != ifname)
            {
                SWSS_LOG_INFO("Interface not match, continue");
                continue;
            }
            int vlan_id = stoi(vlanKey.c_str());
            msg.vlan_id = vlan_id;
            msg.count=1;
            memcpy(&msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
            msg.op_code = L2MCD_OP_ENABLE;
            SWSS_LOG_NOTICE("L2MCD_CFG:MROUTER: [key:%s] %s vlan:%d", l2mc_Mrouter.c_str(),msg.ports[0].pnames,vlan_id);
            sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, sizeof(msg), (void *)&msg);
        }
    }
}

void  L2McMgr::doL2McMrouterUpdateTask(Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    SWSS_LOG_ENTER();

    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
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
        msg.vlan_id = vlan_id;
        msg.count=1;
        memcpy(&msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        if (op == SET_COMMAND)
        {
            msg.op_code = L2MCD_OP_ENABLE;
            if (m_warmstart && findStateMrouterEntry(key))
            {
                (void)m_l2mcMrouterentry.erase(key);
            }
        }
        else
        {
            msg.op_code = L2MCD_OP_DISABLE;
        }
        for (auto i : kfvFieldsValues(t))
        {

        }
        SWSS_LOG_NOTICE("L2MCD_CFG:MROUTER: op:%s [key:%s] %s vlan:%d", op.c_str(), key.c_str(),msg.ports[0].pnames,vlan_id);
        sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_CONFIG_MSG, sizeof(msg), (void *)&msg);
        it = consumer.m_toSync.erase(it);
    }

    if (m_warmstart && consumer.m_toSync.empty())
    {
        l2mcMouterReplayDone = true;
    }
}

void L2McMgr::updateGrpStaticEntry(const string vlan_id, const string ifname)
{
    //m_cfgL2McMrouterTable
    vector<string> l2mcGrpStaticKeys;
    L2MCD_CONFIG_MSG msg;
    SWSS_LOG_ENTER();
    string srcip="0.0.0.0";
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    // "L2MC_STATIC_MEMBER|Vlan20|224.10.10.10|Ethernet69"
    m_cfgL2McStaticTable.getKeys(l2mcGrpStaticKeys);
    if (vlan_id != "")
    {
        for (auto l2mc_GrpStatic : l2mcGrpStaticKeys)
        {
            size_t pos = l2mc_GrpStatic.find('|');
            string vlanKey = l2mc_GrpStatic.substr(4,pos-4);
            string key2 = l2mc_GrpStatic.substr(pos+1);
            pos = key2.find('|');
            string ipKey = key2.substr(0,pos);
            string iname = key2.substr(pos+1);
            msg.op_code = L2MCD_OP_ENABLE;

            SWSS_LOG_INFO("vlanKey %s, vlan_id %s", vlanKey.c_str(), vlan_id.c_str());
            if (vlanKey != vlan_id)
            {
                SWSS_LOG_INFO("Vlan not match, continue");
                continue;
            }
            int vlan_id = stoi(vlanKey.c_str());
            msg.vlan_id = vlan_id;
            msg.count=1;
            memcpy(msg.gaddr,ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
            memcpy(msg.saddr,srcip.c_str(), L2MCD_IP_ADDR_STR_SIZE);
            memcpy(msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
            SWSS_LOG_NOTICE("L2MCD_CFG:STATIC [key:%s]  GA:%s Port:%s vlan:%d", l2mc_GrpStatic.c_str(), ipKey.c_str(), iname.c_str(), vlan_id);
            sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, sizeof(msg), (void *)&msg);
        }
    }
    else if (ifname != "")
    {
        for (auto l2mc_GrpStatic : l2mcGrpStaticKeys)
        {
            size_t pos = l2mc_GrpStatic.find('|');
            string vlanKey = l2mc_GrpStatic.substr(4,pos-4);
            string key2 = l2mc_GrpStatic.substr(pos+1);
            pos = key2.find('|');
            string ipKey = key2.substr(0,pos);
            string iname = key2.substr(pos+1);
            msg.op_code = L2MCD_OP_ENABLE;
            SWSS_LOG_INFO("vlanKey %s, vlan_id %s", vlanKey.c_str(), vlan_id.c_str());
            if (iname != ifname)
            {
                SWSS_LOG_INFO("Interface not match, continue");
                continue;
            }
            int vlan_id = stoi(vlanKey.c_str());
            msg.vlan_id = vlan_id;
            msg.count=1;
            memcpy(msg.gaddr,ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
            memcpy(msg.saddr,srcip.c_str(), L2MCD_IP_ADDR_STR_SIZE);
            memcpy(msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
            SWSS_LOG_NOTICE("L2MCD_CFG:STATIC [key:%s]  GA:%s Port:%s vlan:%d", l2mc_GrpStatic.c_str(), ipKey.c_str(), iname.c_str(), vlan_id);
            sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, sizeof(msg), (void *)&msg);
        }
    }
    
}

void L2McMgr::doL2McStaticEntryTask(Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    SWSS_LOG_ENTER();
    string srcip="0.0.0.0";
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
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
        msg.cmd_code=0;
        int vlan_id = stoi(vlanKey.c_str());
        if (op == SET_COMMAND)
        {
            msg.op_code = L2MCD_OP_ENABLE;
            size_t idx = key.find('|');
            string tmp_key = key.substr(0, idx+1) + srcip + key.substr(idx);
            if (m_warmstart && findStateGrpMemEntry(tmp_key))
            {
                (void)m_l2mcGrpMementry.erase(key);
            }
        }
        else
        {
            msg.op_code = L2MCD_OP_DISABLE;
        }
        msg.vlan_id = vlan_id;
        msg.count=1;
        memcpy(msg.gaddr,ipKey.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        memcpy(msg.saddr,srcip.c_str(), L2MCD_IP_ADDR_STR_SIZE);
        memcpy(msg.ports[0].pnames, iname.c_str(), L2MCD_IFNAME_SIZE);
        for (auto i : kfvFieldsValues(t))
        {
            SWSS_LOG_INFO("L2MCD_CFG Field: %s Val %s vlan:%d opcode:%d", fvField(i).c_str(), fvValue(i).c_str(), vlan_id, msg.op_code );
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:STATIC op:%s [key:%s]  GA:%s Port:%s vlan:%d", op.c_str(), key.c_str(), ipKey.c_str(), iname.c_str(), vlan_id);
        sendMsgL2Mcd(L2MCD_SNOOP_STATIC_CONFIG_MSG, sizeof(msg), (void *)&msg);
        it = consumer.m_toSync.erase(it);
    }

    if (m_warmstart && consumer.m_toSync.empty())
    {
        l2mcGrpMemReplayDone = true;
    }
}

void L2McMgr::doVlanUpdateTask (Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    int j=0;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);
        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            return;
        }

        string vlanKey = key.substr(4); // Remove Vlan prefix
        std::vector<FieldValueTuple> tuples;
        m_cfgL2McGlobalTable.get(key.c_str(), tuples);
        auto its = std::find_if( tuples.begin(), tuples.end(), [](const FieldValueTuple& v){ return v.first == "enabled";} );
        if ( its == tuples.end() ) {
            SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP  %s is not enabled",key.c_str());
            it = consumer.m_toSync.erase(it);
            return;
        }
        
        int vlan_id = stoi(vlanKey.c_str());
        msg.cmd_code=0;
        msg.query_interval=125;
        msg.query_max_response_time=10;
        msg.last_member_query_interval=1000;
        msg.version=2;
        if (op == SET_COMMAND)
        {
            msg.op_code = L2MCD_OP_ENABLE;
            msg.vlan_id = vlan_id;
            for (auto i : tuples)
            {
                if (fvField(i) == "enabled")
                {
                    msg.enabled = (fvValue(i) == "true") ? 1 : 0;
                }
                else if (fvField(i) == "querier")
                {
                    msg.querier = (fvValue(i) == "true") ? 1 : 0;
                }
                else if (fvField(i) == "fast-leave")
                {
                    msg.fast_leave = (fvValue(i) == "true") ? 1 : 0; 
                }
                else if (fvField(i) == "version")
                {
                    msg.version = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "query-interval")
                {
                    msg.query_interval = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "last-member-query-interval")
                {
                    msg.last_member_query_interval = stoi(fvValue(i).c_str());
                }
                else if (fvField(i) == "query-max-response-time")
                {
                    msg.query_max_response_time = stoi(fvValue(i).c_str());
                }
                vector<PORT_ATTR> port_list;
                msg.count =  getVlanMembers(key,port_list);
                j=0;
                for (auto pentry = port_list.begin(); pentry != port_list.end(); pentry++)
                {
                    memcpy(&msg.ports[j++].pnames, pentry->pnames, L2MCD_IFNAME_SIZE);
                    SWSS_LOG_INFO("L2MCD_CFG:SNOOP vlan %s mem-port:%s idx:%d size:%d", key.c_str(), msg.ports[j-1].pnames, j-1, (int)port_list.size());
                }
            }
        }
        else 
        {
            msg.op_code = L2MCD_OP_DISABLE;
            msg.vlan_id = vlan_id;
        }
        SWSS_LOG_NOTICE("L2MCD_CFG:SNOOP %s [key:%s] vlan:%d,Ver:%d, Qry:%d, qI:%d,lmqi:%d,qmr:%d,FL:%d, count:%d", 
               op.c_str(), key.c_str(), vlan_id, msg.version, msg.querier,msg.query_interval,
               msg.last_member_query_interval, msg.query_max_response_time ,
               msg.fast_leave,msg.count);
        sendMsgL2Mcd(L2MCD_SNOOP_CONFIG_MSG, sizeof(msg), (void *)&msg);
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
    Table m_statePortTable(state_db, STATE_PORT_TABLE_NAME);
    Table m_stateLagTable(state_db, STATE_LAG_TABLE_NAME); 
    string Pname; 
    vector<string> portKeys;
    vector<string> lagKeys;
    m_statePortTable.getKeys(portKeys);
    m_stateLagTable.getKeys(lagKeys);
    L2MCD_CONFIG_MSG msg;
    int j=0;

    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    SWSS_LOG_ENTER();
    msg.count=(unsigned int) portKeys.size();
    msg.count+=(unsigned int) lagKeys.size();
    if (msg.count >L2MCD_IPC_MAX_PORTS)
    {
        SWSS_LOG_ERROR("port count %d invalid", msg.count);
        return -1;
    }
    for (auto port_name : portKeys)
    {
        msg.ports[j].oper_state = getPortOperState(port_name);
        memcpy(msg.ports[j++].pnames, port_name.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_INFO("Port:%s oper:%d", port_name.c_str(), msg.ports[j-1].oper_state);
    }
    for (auto lag_name : lagKeys)
    {
        msg.ports[j].oper_state = getPortOperState(lag_name);
        memcpy(msg.ports[j++].pnames, lag_name.c_str(), L2MCD_IFNAME_SIZE);
        SWSS_LOG_INFO("Port:%s oper:%d", lag_name.c_str(), msg.ports[j-1].oper_state);
    }
    msg.op_code = L2MCD_OP_ENABLE;
    SWSS_LOG_NOTICE("L2MCD_CFG:PORTLIST count:%d ",  msg.count);
    sendMsgL2Mcd(L2MCD_SNOOP_PORT_LIST_MSG, sizeof(msg), (void *)&msg);
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
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    // Table vlanmemtable(state_db, STATE_VLAN_MEMBER_TABLE_NAME);
    m_stateVlanMemberTable.getKeys(l2mcVlanMemKeys);


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
        msg.vlan_id = vlanid;
        msg.count =1;
        memcpy(msg.ports[0].pnames, intfName.c_str(), L2MCD_IFNAME_SIZE);
        msg.op_code = L2MCD_OP_ENABLE;
        sendMsgL2Mcd(L2MCD_VLAN_MEM_TABLE_UPDATE, sizeof(msg), (void *)&msg);
    }
}

void L2McMgr::doL2McVlanMemUpdateTask(Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    SWSS_LOG_ENTER();
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
            msg.vlan_id = vlanid;
            msg.count =1;
            memcpy(msg.ports[0].pnames, intfName.c_str(), L2MCD_IFNAME_SIZE);
            if (op == SET_COMMAND)
            {
                msg.op_code = L2MCD_OP_ENABLE;
            }
            else
            {
                msg.op_code = L2MCD_OP_DISABLE;
            }
            SWSS_LOG_NOTICE("L2MCD_CFG:VLAN_MEMBER op:%s iname:%s %s vlan:%d", op.c_str(), msg.ports[0].pnames, intfName.c_str(), msg.vlan_id);
            sendMsgL2Mcd(L2MCD_VLAN_MEM_TABLE_UPDATE, sizeof(msg), (void *)&msg);
        }
        it = consumer.m_toSync.erase(it);
    }
}

void L2McMgr::doL2McLagMemberUpdateTask(Consumer &consumer)
{
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));
    int j=0;
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);

        string po_name;
        string po_mem;
        size_t found = key.find(CONFIGDB_KEY_SEPARATOR);

        if (found != string::npos)
        {
            po_name = key.substr(0, found);
            po_mem  = key.substr(found+1);
            SWSS_LOG_INFO("LAG_MEMBER %s %s %s", po_name.c_str(), po_mem.c_str(), op.c_str());
            memcpy(msg.ports[j++].pnames, po_name.c_str(), L2MCD_IFNAME_SIZE);
            memcpy(msg.ports[j++].pnames, po_mem.c_str(), L2MCD_IFNAME_SIZE);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            msg.op_code = L2MCD_OP_ENABLE;
        }
        else
        {
            msg.op_code = L2MCD_OP_DISABLE;
        }
        msg.count =j;
        SWSS_LOG_NOTICE("L2MCD_CFG:LAG_MEMBER op:%s  Po:%s  count:%d", op.c_str(), po_name.c_str(), msg.count);
        sendMsgL2Mcd(L2MCD_LAG_MEM_TABLE_UPDATE, sizeof(msg), (void *)&msg);
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
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto key = kfvKey(t);
        auto op = kfvOp(t);
        memcpy(msg.ports[0].pnames, key.c_str(), L2MCD_IFNAME_SIZE);
        msg.count=1;
        msg.op_code = (op == SET_COMMAND)? L2MCD_OP_ENABLE:L2MCD_OP_DISABLE;
        msg.ports[0].oper_state = getPortOperState(msg.ports[0].pnames);
        SWSS_LOG_NOTICE("L2MCD_CFG: IF:%s op:%s oper:%d", key.c_str(), op.c_str(),msg.ports[0].oper_state);
        sendMsgL2Mcd(L2MCD_SNOOP_PORT_LIST_MSG, sizeof(msg), (void *)&msg);
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

                if (field == "optimised-multicast-flood" || field == "link-local-groups-suppression")
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
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    SWSS_LOG_ENTER();
    size_t pos=key.find(key_seperator.c_str());
    auto vlan_name = key.substr(0,pos);
    auto pos1 = key.find(key_seperator.c_str(), pos+1);
    auto saddr = key.substr(pos+1, pos1-pos-1);
    auto pos2 = key.find(key_seperator.c_str(), pos1+1);
    auto gaddr = key.substr(pos1+1, pos2-pos1-1);
    auto pos3 = key.find(key_seperator.c_str(), pos2+1);
    auto portname = key.substr(pos2+1, pos3-pos2-1);
    auto pos4 = key.find(key_seperator.c_str(), pos3+1);
    auto type = key.substr(pos3+1, pos4-pos3-1);
    auto pos5 = key.find(key_seperator.c_str(), pos4+1);
    auto leave = key.substr(pos4+1, pos5-pos4-1);
    msg.vlan_id = (unsigned int) stoi(vlan_name.substr(4));
    memcpy(msg.saddr,saddr.c_str(), L2MCD_IP_ADDR_STR_SIZE);
    memcpy(msg.gaddr,gaddr.c_str(), L2MCD_IP_ADDR_STR_SIZE);
    msg.count=1;
    memcpy(msg.ports[0].pnames, portname.c_str(), L2MCD_IFNAME_SIZE);
    if (saddr != "0.0.0.0")
    {
        msg.version = IGMP_VERSION_3;
    }
    else
    {
        msg.version = IGMP_VERSION_2;
    }

    if (op =="SET")
    {
        msg.op_code = L2MCD_OP_ENABLE;
    }
    else 
    {
        msg.op_code = L2MCD_OP_DISABLE;
    }
    msg.cmd_code=cmd_type;
    SWSS_LOG_NOTICE("L2MCD_CFG:REMOTE op:%s [key:%s]  SA:%s GA:%s Port:%s vlan:%d type:%d", 
           op.c_str(), key.c_str(), saddr.c_str(), gaddr.c_str(), portname.c_str(), msg.vlan_id,msg.cmd_code);
    sendMsgL2Mcd(L2MCD_SNOOP_REMOTE_CONFIG_MSG, sizeof(msg), (void *)&msg);
}

void L2McMgr::doL2McProcRemoteMrouterEntries(string op, string key, string key_seperator)
{
    L2MCD_CONFIG_MSG msg;
    memset(&msg, 0, sizeof(L2MCD_CONFIG_MSG));

    SWSS_LOG_ENTER();
    size_t pos=key.find(key_seperator.c_str());
    auto vlan_name = key.substr(0,pos);
    auto pos1 = key.find(key_seperator.c_str(), pos+1);
    auto portname = key.substr(pos+1, pos1-pos-1);
    auto pos2 = key.find(key_seperator.c_str(), pos1+1);
    auto type = key.substr(pos1+1, pos2-pos1-1);
    auto pos3 = key.find(key_seperator.c_str(), pos2+1);
    auto leave = key.substr(pos2+1, pos3-pos2-1);
    msg.vlan_id = (unsigned int) stoi(vlan_name.substr(4));
    msg.count=1;
    memcpy(msg.ports[0].pnames, portname.c_str(), L2MCD_IFNAME_SIZE);
    if (op =="SET")
    {
        msg.op_code = L2MCD_OP_ENABLE;
    }
    else 
    {
        msg.op_code = L2MCD_OP_DISABLE;
    }
    SWSS_LOG_NOTICE("L2MCD_Mrouter:REMOTE op:%s [key:%s] Port:%s vlan:%d", 
           op.c_str(), key.c_str(), portname.c_str(), msg.vlan_id);
    sendMsgL2Mcd(L2MCD_SNOOP_MROUTER_REMOTE_CONFIG_MSG, sizeof(msg), (void *)&msg);
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
        consumer.pop(op, data, values);
        size_t pos = data.find('|');
        string option = data.substr(0,pos);
        string param = data.substr(pos+1);
        string vlan_id = "";
        string ifname = "";
        SWSS_LOG_INFO("Received l2mcd_sync option %s vlan_id %s", option.c_str(), param.c_str());
        if (op == "SNP" && option == "enable")
        {
            vlan_id = param;
            updateVlanMember(vlan_id);
            updateMrouterEntry(vlan_id, ifname);
            updateGrpStaticEntry(vlan_id, ifname);
        }
        else if (op == "LINK_STATUS" && option == "up")
        {
            ifname = param;
            updateMrouterEntry(vlan_id, ifname);
            updateGrpStaticEntry(vlan_id, ifname);
        }
    }
}
